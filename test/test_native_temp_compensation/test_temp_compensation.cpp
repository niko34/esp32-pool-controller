// =============================================================================
// Tests unitaires natifs — Bornes de la compensation T° du pH
// (temp_compensation_logic, bug-114 + feature-068)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / Arduino / Atlas EZO.
// On teste le COMPORTEMENT observable de la logique PURE de
// src/temp_compensation_logic.h (ADR-0017, comme ph_offset / dosing_logic) :
//   clampTempCalOffset()          → offset utilisateur borné à ±5 °C
//   isTempCalOffsetInRange()      → l'offset était-il DÉJÀ dans les bornes
//   isCompensationTempPlausible() → fenêtre [0 ; 40] °C
//   resolveCompensationTemp()     → la CASCADE de repli (le cœur)
//   tempCompStageLabel()          → libellé d'étage exposé au diagnostic MQTT
//
// POURQUOI CES BORNES, ET PAS D'AUTRES — le chiffrage est REFAIT ICI, pas
// recopié (D1.4). Pente de Nernst, verdict `pool-chemistry` du 2026-09-07 :
//   pH_lu − pH_vrai = (pH_vrai − 7) × (T_vrai − T_comp) / T_comp   (T en kelvins)
// Pour un bassin à pH 7,2, cela vaut ≈ 0,7 MILLIÈME de pH par °C d'écart.
// Conséquences, toutes les trois vérifiées par un test :
//   - sonde FIGÉE (feature-068)     → ≈ 0,001 pH  ⇒ négligeable, on instrumente
//                                     et on ne corrige pas ;
//   - offset de calibration +1000   → `RT,1025.0` ⇒ ≈ 0,15 pH ;
//   - offset très négatif           → T_comp → 0 K ⇒ erreur NON BORNÉE.
// Le clamp ±5 °C ferme le deuxième et le troisième. Motif de la valeur : un
// DS18B20 dérive de DIXIÈMES de degré, pas de degrés.
//
// ⚠️ AVERTISSEMENT SUR LA BORNE HAUTE DE 40 °C — À NE PAS PRENDRE POUR UNE
//    VALEUR CONSTRUCTEUR. C'est une borne de PRUDENCE choisie par
//    `pool-chemistry` (≈ 6 °C de marge au-dessus de toute eau de bassin
//    plausible en canicule). La plage réellement admise par la commande `RT`
//    des modules Atlas EZO n'est PAS confirmée par la datasheet : recherche
//    faite dans le dépôt le 2026-09-07, aucune trace (docs/subsystems/sensors.md
//    ne documente que les DÉLAIS de transaction, pas les plages admises).
//    Les tests ci-dessous verrouillent donc un ARBITRAGE DE PROJET, et non une
//    spécification matérielle : si la datasheet est un jour consultée et donne
//    une plage différente, c'est constants.h ET ce fichier qu'il faut reprendre,
//    en repassant par `pool-chemistry` (resserrer sur 15-30 rejetterait des
//    températures VRAIES et dégraderait la compensation au lieu de la protéger).
//
// ⛔ « SUSPENDRE LA COMPENSATION » N'EST PAS UNE OPTION, et la suite le prouve
//    en actes : la cascade rend TOUJOURS une valeur numérique (D4.6). Ne pas
//    envoyer `RT` ferait retomber la lecture sur `R`, où l'EZO conserve SA
//    dernière température interne — silencieusement périmée, invisible et
//    indatable côté firmware. Pire que n'importe quel repli.
//
// Familles :
//   D1 — clamp de l'offset (±5 °C, bornes INCLUSIVES) + chiffrage de l'erreur pH
//   D2 — isTempCalOffsetInRange, et sa relation à clampTempCalOffset
//   D3 — fenêtre de plausibilité [0 ; 40] °C + sentinelles réelles du capteur
//   D4 — la CASCADE : chaque étage, chaque transition, la dégradation en chaîne
//   D5 — libellé d'étage ↔ étage rendu (un décalage rendrait le diagnostic
//        MQTT `<base>/diagnostic` trompeur : ezo_comp_temp / ezo_comp_stage)
//   D6 — NON-RÉGRESSION exigée par le verdict chimique : température figée ⇒
//        `RT` inchangé, planning de filtration inchangé, dosage inchangé
//   D7 — sentinelles STRUCTURELLES : les QUATRE points de clamp, et l'absence de
//        tout couplage entre le drapeau « figé » et une décision.
//        ⚠️ Ce compte a été annoncé « deux », puis « trois », avant d'être grepé
//        et fixé à quatre — y compris dans cet en-tête, corrigé le 2026-09-07.
//        Les assertions, elles, visent chaque site DANS SA FONCTION sur du code
//        DÉPOUILLÉ de ses commentaires : c'est la seule version du compte qui
//        ne puisse pas mentir. Si cet en-tête et les assertions divergent un
//        jour, croire les assertions (ADR-0073).
// =============================================================================

#include <unity.h>
#include <math.h>    // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include <stdio.h>   // snprintf / fopen (messages de balayage, sentinelles D7)
#include <stdlib.h>  // malloc / free (lecture des fichiers du dépôt, D7)
#include <string.h>

#include "dosing_logic.h"             // D6 : DoseInputs (aucune entrée de T°)
#include "schedule_logic.h"           // D6 : computeAutoWindow (planning auto)
#include "temp_compensation_logic.h"  // le module sous test

void setUp(void) {}
void tearDown(void) {}

// Sentinelles RÉELLES que la chaîne de mesure produit, valeurs figées ici pour
// rester en natif (DallasTemperature.h n'est pas disponible hors cible) :
//   - 85.0 °C : valeur de POWER-ON RESET du registre de scratchpad du DS18B20.
//     Un capteur qui vient d'être alimenté et n'a pas encore converti la rend
//     telle quelle — c'est une lecture SYNTAXIQUEMENT valide et physiquement
//     fausse, donc exactement ce que la fenêtre de plausibilité doit rejeter.
//   - -127.0 °C : DEVICE_DISCONNECTED_C de DallasTemperature
//     (.pio/libdeps/*/DallasTemperature/DallasTemperature.h:37, `-127`).
static const float kDs18b20PowerOnResetC = 85.0f;
static const float kDeviceDisconnectedC  = -127.0f;

// Fenêtre BRUTE du capteur, rejetée par la fenêtre de compensation : les
// lectures DS18B20 sont bornées à (−55 ; 125) °C côté sensors.cpp, ce qui
// laissait passer des valeurs à 0,05-0,07 pH d'erreur. La fenêtre [0 ; 40] est
// STRICTEMENT plus serrée, et c'est le but.
static const float kDs18b20RangeMinC = -55.0f;
static const float kDs18b20RangeMaxC = 125.0f;

// Erreur de pH induite par une compensation à `tempCompC` alors que l'eau est
// réellement à `tempTrueC`, pour un bassin à `phTrue`. Pente de Nernst, formule
// de la spec, T en KELVINS. Sert à chiffrer ce que les bornes achètent.
static float phErrorFromCompensation(float phTrue, float tempTrueC, float tempCompC) {
  const float tTrueK = tempTrueC + 273.15f;
  const float tCompK = tempCompC + 273.15f;
  return (phTrue - 7.0f) * (tTrueK - tCompK) / tCompK;
}

// =============================================================================
// D1 — clampTempCalOffset : l'offset utilisateur est borné à ±5 °C
// =============================================================================

// --- D1.1 : régime normal, l'écrêtage ne doit RIEN toucher ---

static void test_clamp_offset_inside_range_is_identity(void) {
  // Les valeurs qu'un opérateur pose réellement : des dixièmes de degré.
  TEST_ASSERT_EQUAL_FLOAT(0.0f,  clampTempCalOffset(0.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.1f,  clampTempCalOffset(0.1f));
  TEST_ASSERT_EQUAL_FLOAT(-0.3f, clampTempCalOffset(-0.3f));
  TEST_ASSERT_EQUAL_FLOAT(1.25f, clampTempCalOffset(1.25f));
  TEST_ASSERT_EQUAL_FLOAT(-2.5f, clampTempCalOffset(-2.5f));
}

static void test_clamp_offset_at_exact_lower_bound_is_kept(void) {
  // Borne INCLUSIVE : le code n'écrête que sur `offset < kTempCalOffsetMin`,
  // donc −5,0 passe INCHANGÉ (et non « ramené à −5 », ce qui serait le même
  // nombre mais pas la même règle : cf. isTempCalOffsetInRange ci-dessous).
  TEST_ASSERT_EQUAL_FLOAT(kTempCalOffsetMin, clampTempCalOffset(kTempCalOffsetMin));
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, clampTempCalOffset(-5.0f));
}

static void test_clamp_offset_at_exact_upper_bound_is_kept(void) {
  TEST_ASSERT_EQUAL_FLOAT(kTempCalOffsetMax, clampTempCalOffset(kTempCalOffsetMax));
  TEST_ASSERT_EQUAL_FLOAT(5.0f, clampTempCalOffset(5.0f));
}

static void test_clamp_offset_just_inside_bounds_is_kept(void) {
  TEST_ASSERT_EQUAL_FLOAT(4.99f,  clampTempCalOffset(4.99f));
  TEST_ASSERT_EQUAL_FLOAT(-4.99f, clampTempCalOffset(-4.99f));
}

// --- D1.2 : hors bornes, l'écrêtage doit ramener À la borne ---

static void test_clamp_offset_just_outside_bounds_is_clamped(void) {
  TEST_ASSERT_EQUAL_FLOAT(kTempCalOffsetMax, clampTempCalOffset(5.01f));
  TEST_ASSERT_EQUAL_FLOAT(kTempCalOffsetMin, clampTempCalOffset(-5.01f));
}

static void test_clamp_offset_far_beyond_upper_is_clamped(void) {
  // LE cas de la spec : +1000 produisait `RT,1025.0`.
  TEST_ASSERT_EQUAL_FLOAT(5.0f, clampTempCalOffset(1000.0f));
  TEST_ASSERT_EQUAL_FLOAT(5.0f, clampTempCalOffset(50.0f));
  TEST_ASSERT_EQUAL_FLOAT(5.0f, clampTempCalOffset(100000.0f));
}

static void test_clamp_offset_far_beyond_lower_is_clamped(void) {
  // Le cas NON BORNÉ : un offset très négatif fait tendre T_comp vers 0 K.
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, clampTempCalOffset(-1000.0f));
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, clampTempCalOffset(-50.0f));
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, clampTempCalOffset(-273.15f));
}

static void test_clamp_offset_nan_is_neutralized_to_zero(void) {
  // Fail-safe : mieux vaut AUCUNE correction qu'une correction indéterminée.
  // Un NaN qui ressortirait NaN contaminerait `RT` et rendrait la mesure de pH
  // inexploitable sans le dire.
  const float out = clampTempCalOffset(NAN);
  TEST_ASSERT_FALSE_MESSAGE(isnan(out), "Un offset NaN doit etre neutralise, pas propage.");
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out);
}

static void test_clamp_offset_infinities_are_clamped(void) {
  TEST_ASSERT_EQUAL_FLOAT(kTempCalOffsetMax, clampTempCalOffset(INFINITY));
  TEST_ASSERT_EQUAL_FLOAT(kTempCalOffsetMin, clampTempCalOffset(-INFINITY));
}

// --- D1.3 : propriétés ---

static void test_clamp_offset_is_idempotent(void) {
  // Conséquence directe des TROIS points de clamp de la production (écriture /
  // chargement NVS / émission) : appliquer l'écrêtage plusieurs fois de suite
  // doit être sans effet, sinon la défense en profondeur déformerait la valeur.
  const float probes[9] = {0.0f, 0.4f, -0.4f, 5.0f, -5.0f,
                           5.01f, -5.01f, 1000.0f, NAN};
  for (int i = 0; i < 9; ++i) {
    const float once  = clampTempCalOffset(probes[i]);
    const float twice = clampTempCalOffset(once);
    char msg[96];
    snprintf(msg, sizeof(msg), "Clamp non idempotent pour l'entree %g", (double)probes[i]);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(once, twice, msg);
  }
}

static void test_clamp_offset_output_is_always_within_bounds(void) {
  // Balayage large : la SORTIE est toujours dans les bornes, quoi qu'on entre.
  for (int i = -2000; i <= 2000; ++i) {
    const float in  = (float)i * 0.5f;  // −1000 … +1000 par pas de 0,5
    const float out = clampTempCalOffset(in);
    char msg[112];
    snprintf(msg, sizeof(msg), "Sortie %g hors bornes pour l'entree %g",
             (double)out, (double)in);
    TEST_ASSERT_TRUE_MESSAGE(out >= kTempCalOffsetMin && out <= kTempCalOffsetMax, msg);
  }
}

static void test_offset_bounds_constants_are_symmetric_five_degrees(void) {
  // Verrouille la VALEUR, pas seulement la mécanique : ±5 °C. Motif dans
  // constants.h — un DS18B20 dérive de dixièmes de degré, pas de degrés.
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, kTempCalOffsetMin);
  TEST_ASSERT_EQUAL_FLOAT(5.0f,  kTempCalOffsetMax);
  TEST_ASSERT_EQUAL_FLOAT(0.0f,  kTempCalOffsetMin + kTempCalOffsetMax);
}

// --- D1.4 : ce que le clamp achète, en pH (chiffrage REFAIT) ---

static void test_bounded_offset_keeps_ph_error_under_the_tightest_deadband(void) {
  // Bassin à pH 7,2, eau réellement à 25 °C. Pire cas ADMIS après écrêtage :
  // l'offset vaut ±5 °C, donc T_comp = 20 ou 30 °C.
  const float phTrue = 7.2f;
  const float tTrue  = 25.0f;
  const float errHigh = phErrorFromCompensation(phTrue, tTrue, tTrue + kTempCalOffsetMax);
  const float errLow  = phErrorFromCompensation(phTrue, tTrue, tTrue + kTempCalOffsetMin);
  char msg[128];
  snprintf(msg, sizeof(msg), "Erreur pH au clamp haut = %g (attendu < 0.01)", (double)errHigh);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(errHigh) < 0.01f, msg);
  snprintf(msg, sizeof(msg), "Erreur pH au clamp bas = %g (attendu < 0.01)", (double)errLow);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(errLow) < 0.01f, msg);
  // ≈ 0,7 millième de pH par °C : l'ordre de grandeur de la spec est bien celui-là.
  TEST_ASSERT_FLOAT_WITHIN(0.0004f, 0.0007f, fabsf(errHigh) / kTempCalOffsetMax);
}

static void test_unbounded_positive_offset_would_cost_about_fifteen_hundredths_of_ph(void) {
  // Le défaut d'origine : offset +1000 ⇒ `RT,1025.0`. On retrouve bien le
  // ≈ +0,15 pH de la spec, soit QUINZE fois la bande morte la plus serrée.
  const float err = phErrorFromCompensation(7.2f, 25.0f, 1025.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.154f, fabsf(err));
  TEST_ASSERT_TRUE_MESSAGE(fabsf(err) > 15.0f * 0.01f,
                           "Le cas +1000 doit depasser 15x la bande morte 0,01.");
}

static void test_unbounded_negative_offset_error_is_not_bounded(void) {
  // T_comp → 0 K : l'erreur diverge. On le montre en s'en approchant.
  const float err30K  = phErrorFromCompensation(7.2f, 25.0f, 30.0f - 273.15f);
  const float err5K   = phErrorFromCompensation(7.2f, 25.0f, 5.0f - 273.15f);
  const float err0p5K = phErrorFromCompensation(7.2f, 25.0f, 0.5f - 273.15f);
  TEST_ASSERT_TRUE(fabsf(err30K) > 1.0f);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(err5K) > fabsf(err30K), "L'erreur doit croitre vers 0 K.");
  TEST_ASSERT_TRUE_MESSAGE(fabsf(err0p5K) > fabsf(err5K), "L'erreur doit diverger vers 0 K.");
  TEST_ASSERT_TRUE_MESSAGE(fabsf(err0p5K) > 100.0f,
                           "Pres de 0 K l'erreur doit etre absurde (non bornee).");
  // Et le clamp interdit d'y arriver : aucun offset écrêté ne peut faire
  // descendre T_comp sous le zéro absolu, ni même sous 0 °C pour une eau à 25.
  TEST_ASSERT_TRUE(25.0f + clampTempCalOffset(-1000.0f) > 0.0f);
}

// =============================================================================
// D2 — isTempCalOffsetInRange : « la valeur de l'opérateur a-t-elle été touchée »
// =============================================================================
// Cette fonction n'est pas décorative : c'est elle qui décide si le firmware
// JOURNALISE un écrêtage. Un utilisateur doit savoir que sa saisie a été bornée
// (route de configuration) et un exploitant doit savoir que la NVS était polluée
// par une version antérieure (chargement au boot).

static void test_offset_in_range_accepts_realistic_values(void) {
  TEST_ASSERT_TRUE(isTempCalOffsetInRange(0.0f));
  TEST_ASSERT_TRUE(isTempCalOffsetInRange(0.2f));
  TEST_ASSERT_TRUE(isTempCalOffsetInRange(-0.2f));
  TEST_ASSERT_TRUE(isTempCalOffsetInRange(3.0f));
}

static void test_offset_in_range_at_exact_bounds_is_true(void) {
  // Bornes INCLUSIVES des deux côtés : ±5,0 est ACCEPTÉ, donc NON journalisé.
  TEST_ASSERT_TRUE_MESSAGE(isTempCalOffsetInRange(kTempCalOffsetMin),
                           "La borne basse doit etre INCLUSIVE.");
  TEST_ASSERT_TRUE_MESSAGE(isTempCalOffsetInRange(kTempCalOffsetMax),
                           "La borne haute doit etre INCLUSIVE.");
}

static void test_offset_out_of_range_just_outside_is_false(void) {
  TEST_ASSERT_FALSE(isTempCalOffsetInRange(5.01f));
  TEST_ASSERT_FALSE(isTempCalOffsetInRange(-5.01f));
  TEST_ASSERT_FALSE(isTempCalOffsetInRange(1000.0f));
  TEST_ASSERT_FALSE(isTempCalOffsetInRange(-1000.0f));
}

static void test_offset_in_range_rejects_nan_and_infinities(void) {
  TEST_ASSERT_FALSE_MESSAGE(isTempCalOffsetInRange(NAN),
                            "NaN n'est pas 'dans les bornes' : l'ecretage doit etre signale.");
  TEST_ASSERT_FALSE(isTempCalOffsetInRange(INFINITY));
  TEST_ASSERT_FALSE(isTempCalOffsetInRange(-INFINITY));
}

static void test_in_range_is_exactly_the_clamp_being_identity(void) {
  // Le contrat entre les deux fonctions : isTempCalOffsetInRange(x) est vrai
  // SI ET SEULEMENT SI clampTempCalOffset(x) == x. Si l'une des deux dérive de
  // l'autre, le firmware journalise un écrêtage qui n'a pas eu lieu, ou —
  // beaucoup plus grave — écrête en silence.
  for (int i = -700; i <= 700; ++i) {
    const float x = (float)i * 0.01f;  // −7,00 … +7,00 par centièmes
    const bool inRange = isTempCalOffsetInRange(x);
    const bool identity = (clampTempCalOffset(x) == x);
    char msg[128];
    snprintf(msg, sizeof(msg), "Desaccord in_range/clamp pour %g (in_range=%d, identity=%d)",
             (double)x, (int)inRange, (int)identity);
    TEST_ASSERT_EQUAL_MESSAGE(identity ? 1 : 0, inRange ? 1 : 0, msg);
  }
  // NaN : seul cas où « clamp == x » est faux par construction IEEE-754, et où
  // in_range doit être faux lui aussi — les deux concordent donc encore.
  TEST_ASSERT_FALSE(isTempCalOffsetInRange(NAN));
  TEST_ASSERT_FALSE(clampTempCalOffset(NAN) != clampTempCalOffset(NAN));
}

// =============================================================================
// D3 — isCompensationTempPlausible : fenêtre [0 ; 40] °C
// =============================================================================

static void test_plausible_accepts_the_whole_operating_range(void) {
  // Hors saison comme en canicule : ces valeurs sont VRAIES et doivent passer.
  const float ok[7] = {1.0f, 4.0f, 12.0f, 20.0f, 28.0f, 34.0f, 39.5f};
  for (int i = 0; i < 7; ++i) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%g degres est une temperature de bassin PLAUSIBLE", (double)ok[i]);
    TEST_ASSERT_TRUE_MESSAGE(isCompensationTempPlausible(ok[i]), msg);
  }
}

static void test_plausible_at_exact_bounds_is_true(void) {
  // Bornes INCLUSIVES : 0 °C et 40 °C sont ACCEPTÉES.
  // Rappel : 40 °C est une borne de PRUDENCE de projet, PAS une valeur de la
  // datasheet Atlas (cf. l'avertissement en tête de fichier).
  TEST_ASSERT_TRUE_MESSAGE(isCompensationTempPlausible(kEzoCompTempMinC),
                           "0 degre doit etre ACCEPTE (borne inclusive).");
  TEST_ASSERT_TRUE_MESSAGE(isCompensationTempPlausible(kEzoCompTempMaxC),
                           "40 degres doit etre ACCEPTE (borne inclusive).");
  TEST_ASSERT_TRUE(isCompensationTempPlausible(0.0f));
  TEST_ASSERT_TRUE(isCompensationTempPlausible(40.0f));
}

static void test_plausible_just_inside_bounds_is_true(void) {
  TEST_ASSERT_TRUE(isCompensationTempPlausible(0.01f));
  TEST_ASSERT_TRUE(isCompensationTempPlausible(39.99f));
}

static void test_implausible_just_outside_bounds_is_false(void) {
  TEST_ASSERT_FALSE_MESSAGE(isCompensationTempPlausible(-0.01f),
                            "Sous 0 degre (gel) la mesure n'a plus de sens.");
  TEST_ASSERT_FALSE_MESSAGE(isCompensationTempPlausible(40.01f),
                            "Au-dela de 40 degres, refus (borne de prudence projet).");
}

static void test_implausible_rejects_nan(void) {
  TEST_ASSERT_FALSE_MESSAGE(isCompensationTempPlausible(NAN),
                            "NaN n'est JAMAIS plausible : il ne doit pas partir dans RT.");
}

static void test_the_nan_guard_of_the_plausibility_window_is_belt_and_braces(void) {
  // CAMPAGNE DE MUTATION — MUTANT ÉQUIVALENT, consigné ici pour qu'il ne soit
  // pas « réparé » en affaiblissant un test.
  //
  // Supprimer purement et simplement `if (isnan(tempC)) return false;` de
  // isCompensationTempPlausible() ne change AUCUN comportement observable :
  // IEEE-754 impose que toute comparaison d'ordre avec NaN soit FAUSSE, donc
  // `NaN >= kEzoCompTempMinC` est déjà faux et la fonction rend déjà false.
  // Ce mutant est donc ÉQUIVALENT au code d'origine, et aucun test ne peut le
  // tuer — ce n'est pas un trou de couverture. La conclusion de la campagne
  // reste inchangée : le contrat « NaN n'est jamais plausible » est verrouillé
  // (test_implausible_rejects_nan), et le mutant OBSERVABLE de même intention
  // — `if (isnan(tempC)) return true;` — est tué par 16 cas.
  //
  // Le test ci-dessous vérifie la PRÉMISSE de ce raisonnement sur la machine
  // qui exécute la suite, au lieu de la supposer. Si un jour un `-ffast-math`
  // (ou un `-ffinite-math-only`) était ajouté au build, cette prémisse
  // tomberait, ce test tomberait avec elle — et le garde `isnan` redeviendrait
  // LOAD-BEARING. C'est précisément l'alerte qu'on veut.
  const volatile float nanValue = NAN;
  TEST_ASSERT_FALSE_MESSAGE(nanValue >= kEzoCompTempMinC,
      "IEEE-754 viole : NaN >= borne est VRAI. Le garde isnan() de la fenetre de "
      "plausibilite devient alors indispensable (verifier -ffast-math).");
  TEST_ASSERT_FALSE_MESSAGE(nanValue <= kEzoCompTempMaxC,
      "IEEE-754 viole : NaN <= borne est VRAI (verifier -ffast-math).");
  // Le garde de clampTempCalOffset(), lui, est LOAD-BEARING et non redondant :
  // sans lui, un offset NaN ressortirait NaN (les deux comparaisons échouant,
  // la fonction rendrait `offset` tel quel) et contaminerait la mesure.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, clampTempCalOffset(NAN),
      "Le garde NaN du clamp d'offset est indispensable, pas decoratif.");
}

static void test_implausible_rejects_infinities(void) {
  TEST_ASSERT_FALSE(isCompensationTempPlausible(INFINITY));
  TEST_ASSERT_FALSE(isCompensationTempPlausible(-INFINITY));
}

static void test_implausible_rejects_ds18b20_power_on_reset_sentinel(void) {
  // 85,0 °C = registre de scratchpad non converti. Lecture syntaxiquement
  // valide, physiquement fausse : c'est LA sentinelle du DS18B20.
  TEST_ASSERT_FALSE_MESSAGE(isCompensationTempPlausible(kDs18b20PowerOnResetC),
                            "85.0 (power-on reset DS18B20) doit etre REJETE.");
}

static void test_implausible_rejects_device_disconnected_sentinel(void) {
  // DEVICE_DISCONNECTED_C = −127 (DallasTemperature.h:37).
  TEST_ASSERT_FALSE_MESSAGE(isCompensationTempPlausible(kDeviceDisconnectedC),
                            "DEVICE_DISCONNECTED_C (-127) doit etre REJETE.");
}

static void test_compensation_window_is_strictly_tighter_than_the_sensor_range(void) {
  // Les lectures DS18B20 étaient déjà bornées à (−55 ; 125) °C — insuffisant :
  // même aux bornes de CETTE fenêtre l'erreur de pH atteint 0,05-0,07. La
  // fenêtre de compensation doit donc rejeter les deux extrémités brutes.
  TEST_ASSERT_FALSE_MESSAGE(isCompensationTempPlausible(kDs18b20RangeMinC),
                            "-55 (borne brute capteur) doit etre REJETE par la compensation.");
  TEST_ASSERT_FALSE_MESSAGE(isCompensationTempPlausible(kDs18b20RangeMaxC),
                            "125 (borne brute capteur) doit etre REJETE par la compensation.");
  TEST_ASSERT_TRUE(kEzoCompTempMinC > kDs18b20RangeMinC);
  TEST_ASSERT_TRUE(kEzoCompTempMaxC < kDs18b20RangeMaxC);
}

static void test_implausible_rejects_the_unbounded_offset_result(void) {
  // 25 + 1000 = 1025 : la valeur exacte qui partait dans `RT,1025.0`.
  TEST_ASSERT_FALSE(isCompensationTempPlausible(1025.0f));
  TEST_ASSERT_FALSE(isCompensationTempPlausible(-975.0f));
}

static void test_compensation_bounds_constants_are_zero_and_forty(void) {
  // ⚠️ 40 °C = borne de PRUDENCE `pool-chemistry`, NON confirmée par la
  // datasheet Atlas. Ce test verrouille l'arbitrage du dépôt pour qu'un
  // resserrement (15-30, qui rejetterait des températures vraies) ou un
  // élargissement silencieux échoue bruyamment.
  TEST_ASSERT_EQUAL_FLOAT(0.0f,  kEzoCompTempMinC);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, kEzoCompTempMaxC);
  TEST_ASSERT_TRUE(kEzoCompTempMinC < kEzoCompTempMaxC);
}

static void test_plausibility_sweep_matches_the_closed_interval(void) {
  // Balayage au centième sur −10 … +50 : la fonction doit être EXACTEMENT
  // l'intervalle fermé [0 ; 40], sans trou ni débordement.
  for (int i = -1000; i <= 5000; ++i) {
    const float t = (float)i * 0.01f;
    const bool expected = (t >= kEzoCompTempMinC) && (t <= kEzoCompTempMaxC);
    char msg[112];
    snprintf(msg, sizeof(msg), "Plausibilite incorrecte pour %g degres", (double)t);
    TEST_ASSERT_EQUAL_MESSAGE(expected ? 1 : 0,
                              isCompensationTempPlausible(t) ? 1 : 0, msg);
  }
}

// =============================================================================
// D4 — La CASCADE : c'est le cœur
// =============================================================================
//   1. mesure courante plausible                                    → Current
//   2. sinon dernière valeur plausible ET âge sous la fenêtre stale  → LastPlausible
//   3. sinon kEzoFallbackTempC                                       → Fallback

static const uint32_t kStale = kSensorStaleTimeoutMs;  // 20 s = 10 lectures DS18B20

// --- D4.1 : étage 1, la mesure courante ---

static void test_cascade_uses_the_current_measurement_when_plausible(void) {
  const TempCompDecision d = resolveCompensationTemp(26.4f, 25.0f, 0, kStale);
  TEST_ASSERT_EQUAL_FLOAT(26.4f, d.tempC);
  TEST_ASSERT_EQUAL(TempCompStage::Current, d.stage);
}

static void test_cascade_current_ignores_last_plausible_and_its_age(void) {
  // Une mesure plausible court-circuite tout : ni la valeur mémorisée ni son âge
  // ne doivent pouvoir la remplacer.
  const TempCompDecision fresh = resolveCompensationTemp(26.4f, 12.0f, 0, kStale);
  const TempCompDecision stale = resolveCompensationTemp(26.4f, 12.0f, 0xFFFFFFFFu, kStale);
  const TempCompDecision noHist = resolveCompensationTemp(26.4f, NAN, 0, kStale);
  TEST_ASSERT_EQUAL_FLOAT(26.4f, fresh.tempC);
  TEST_ASSERT_EQUAL_FLOAT(26.4f, stale.tempC);
  TEST_ASSERT_EQUAL_FLOAT(26.4f, noHist.tempC);
  TEST_ASSERT_EQUAL(TempCompStage::Current, fresh.stage);
  TEST_ASSERT_EQUAL(TempCompStage::Current, stale.stage);
  TEST_ASSERT_EQUAL(TempCompStage::Current, noHist.stage);
}

static void test_cascade_current_at_exact_plausibility_bounds(void) {
  const TempCompDecision lo = resolveCompensationTemp(kEzoCompTempMinC, 25.0f, 0, kStale);
  const TempCompDecision hi = resolveCompensationTemp(kEzoCompTempMaxC, 25.0f, 0, kStale);
  TEST_ASSERT_EQUAL(TempCompStage::Current, lo.stage);
  TEST_ASSERT_EQUAL(TempCompStage::Current, hi.stage);
  TEST_ASSERT_EQUAL_FLOAT(0.0f,  lo.tempC);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, hi.tempC);
}

// --- D4.2 : étage 2, la dernière valeur plausible ---

static void test_cascade_falls_back_to_last_plausible_when_measure_is_nan(void) {
  const TempCompDecision d = resolveCompensationTemp(NAN, 27.3f, 5000, kStale);
  TEST_ASSERT_EQUAL_FLOAT(27.3f, d.tempC);
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, d.stage);
}

static void test_cascade_falls_back_to_last_plausible_for_each_sentinel(void) {
  // Les trois falsifications réelles : power-on reset, sonde débranchée, et le
  // résultat d'un offset non borné (qui ne peut plus arriver, mais la cascade
  // doit rester correcte si un autre chemin le produisait).
  const float bad[5] = {kDs18b20PowerOnResetC, kDeviceDisconnectedC, 1025.0f, -0.5f, 41.0f};
  for (int i = 0; i < 5; ++i) {
    const TempCompDecision d = resolveCompensationTemp(bad[i], 24.8f, 1000, kStale);
    char msg[112];
    snprintf(msg, sizeof(msg), "Mesure %g : l'etage 2 doit servir 24.8", (double)bad[i]);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(24.8f, d.tempC, msg);
    TEST_ASSERT_EQUAL_MESSAGE(TempCompStage::LastPlausible, d.stage, msg);
  }
}

static void test_cascade_last_plausible_accepted_at_exact_stale_window(void) {
  // Fenêtre INCLUSIVE : âge == kSensorStaleTimeoutMs est encore FRAIS.
  const TempCompDecision d = resolveCompensationTemp(NAN, 21.0f, kStale, kStale);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(21.0f, d.tempC,
      "A l'age EXACT de la fenetre stale, l'etage 2 doit encore servir.");
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, d.stage);
}

static void test_cascade_last_plausible_refused_just_past_stale_window(void) {
  // Une milliseconde de plus et la valeur est PÉRIMÉE : c'est tout l'objet de
  // la fenêtre. Servir une température de 20 s d'âge est acceptable ; servir
  // celle d'il y a une heure, non — c'est un repli déguisé en mesure.
  const TempCompDecision d = resolveCompensationTemp(NAN, 21.0f, kStale + 1, kStale);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(kEzoFallbackTempC, d.tempC,
      "Passe la fenetre stale, l'etage 2 doit etre REFUSE.");
  TEST_ASSERT_EQUAL(TempCompStage::Fallback, d.stage);
}

static void test_cascade_last_plausible_at_zero_age_is_fresh(void) {
  const TempCompDecision d = resolveCompensationTemp(NAN, 19.5f, 0, kStale);
  TEST_ASSERT_EQUAL_FLOAT(19.5f, d.tempC);
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, d.stage);
}

static void test_cascade_last_plausible_must_itself_be_plausible(void) {
  // L'étage 2 n'est pas un passe-droit : la valeur mémorisée est RE-VALIDÉE.
  const float bad[4] = {kDs18b20PowerOnResetC, kDeviceDisconnectedC, -1.0f, 40.5f};
  for (int i = 0; i < 4; ++i) {
    const TempCompDecision d = resolveCompensationTemp(NAN, bad[i], 0, kStale);
    char msg[128];
    snprintf(msg, sizeof(msg),
             "Une derniere valeur invalide (%g) ne doit pas etre servie", (double)bad[i]);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(kEzoFallbackTempC, d.tempC, msg);
    TEST_ASSERT_EQUAL_MESSAGE(TempCompStage::Fallback, d.stage, msg);
  }
}

static void test_cascade_last_plausible_at_exact_bounds_is_served(void) {
  const TempCompDecision lo = resolveCompensationTemp(NAN, kEzoCompTempMinC, 0, kStale);
  const TempCompDecision hi = resolveCompensationTemp(NAN, kEzoCompTempMaxC, 0, kStale);
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, lo.stage);
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, hi.stage);
}

// --- D4.3 : étage 3, le repli, et la DÉGRADATION EN CHAÎNE ---

static void test_cascade_chained_degradation_ends_on_fallback(void) {
  // LE cas qui compte : mesure courante invalide ET dernière valeur plausible
  // trop vieille ⇒ repli, avec l'étage Fallback (et pas LastPlausible, qui
  // ferait croire à une mesure récente).
  const TempCompDecision d =
      resolveCompensationTemp(kDs18b20PowerOnResetC, 26.0f, kStale + 60000, kStale);
  TEST_ASSERT_EQUAL_FLOAT(kEzoFallbackTempC, d.tempC);
  TEST_ASSERT_EQUAL_MESSAGE(TempCompStage::Fallback, d.stage,
      "Degradation en chaine : l'etage rendu doit etre Fallback, pas LastPlausible.");
}

static void test_cascade_fallback_when_nothing_is_available(void) {
  // Boot avec sonde absente : aucune mesure, aucun historique.
  const TempCompDecision d = resolveCompensationTemp(NAN, NAN, 0xFFFFFFFFu, kStale);
  TEST_ASSERT_EQUAL_FLOAT(kEzoFallbackTempC, d.tempC);
  TEST_ASSERT_EQUAL(TempCompStage::Fallback, d.stage);
}

static void test_cascade_fallback_value_is_twentyfive_degrees(void) {
  TEST_ASSERT_EQUAL_FLOAT(25.0f, kEzoFallbackTempC);
  // Et ce repli est chimiquement acceptable : ≤ 0,01 pH sur toute la plage
  // d'exploitation d'un bassin (verdict pool-chemistry).
  for (int t = 10; t <= 35; ++t) {
    const float err = phErrorFromCompensation(7.2f, (float)t, kEzoFallbackTempC);
    char msg[128];
    snprintf(msg, sizeof(msg), "Repli 25 degres a %d degres reels : erreur pH %g",
             t, (double)err);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(err) <= 0.011f, msg);
  }
}

static void test_cascade_fallback_is_plausible_by_construction(void) {
  // Le repli ne doit pas être une valeur que la fenêtre rejetterait : sinon la
  // cascade se contredirait elle-même.
  TEST_ASSERT_TRUE(isCompensationTempPlausible(kEzoFallbackTempC));
}

// --- D4.4 : les TRANSITIONS ---

static void test_cascade_transition_current_then_last_then_fallback(void) {
  // Scénario d'une sonde qui décroche : mesure OK → mesure perdue mais mémoire
  // fraîche → mémoire périmée. Les trois étages doivent se succéder DANS CET
  // ORDRE, et jamais sauter le deuxième.
  const TempCompDecision s1 = resolveCompensationTemp(26.0f, 25.9f, 0, kStale);
  const TempCompDecision s2 = resolveCompensationTemp(NAN, 26.0f, 5000, kStale);
  const TempCompDecision s3 = resolveCompensationTemp(NAN, 26.0f, kStale + 1, kStale);
  TEST_ASSERT_EQUAL(TempCompStage::Current,       s1.stage);
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, s2.stage);
  TEST_ASSERT_EQUAL(TempCompStage::Fallback,      s3.stage);
  TEST_ASSERT_EQUAL_FLOAT(26.0f, s1.tempC);
  TEST_ASSERT_EQUAL_FLOAT(26.0f, s2.tempC);
  TEST_ASSERT_EQUAL_FLOAT(kEzoFallbackTempC, s3.tempC);
}

static void test_cascade_transition_back_from_fallback_to_current(void) {
  // La sonde revient : on doit RESSORTIR du repli immédiatement, sans hystérésis
  // cachée — un repli qui collerait resterait invisible dans les logs.
  const TempCompDecision down = resolveCompensationTemp(NAN, NAN, 0xFFFFFFFFu, kStale);
  const TempCompDecision up   = resolveCompensationTemp(22.2f, NAN, 0xFFFFFFFFu, kStale);
  TEST_ASSERT_EQUAL(TempCompStage::Fallback, down.stage);
  TEST_ASSERT_EQUAL(TempCompStage::Current,  up.stage);
  TEST_ASSERT_EQUAL_FLOAT(22.2f, up.tempC);
}

static void test_cascade_transition_last_plausible_back_to_current(void) {
  const TempCompDecision sub = resolveCompensationTemp(NAN, 23.0f, 1000, kStale);
  const TempCompDecision ok  = resolveCompensationTemp(23.4f, 23.0f, 1000, kStale);
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, sub.stage);
  TEST_ASSERT_EQUAL(TempCompStage::Current, ok.stage);
}

// --- D4.5 : l'ordre de la cascade, isolé ---

static void test_cascade_prefers_last_plausible_over_fallback(void) {
  // Sentinelle d'ORDRE : quand l'étage 2 est disponible, le repli ne doit PAS
  // être servi. Inverser les deux rendrait le repli 25 °C indiscernable d'une
  // mesure récente et ferait perdre l'information de fraîcheur.
  const TempCompDecision d = resolveCompensationTemp(NAN, 31.0f, 1, kStale);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(31.0f, d.tempC,
      "L'etage 2 est prioritaire sur le repli quand il est frais et plausible.");
  TEST_ASSERT_EQUAL(TempCompStage::LastPlausible, d.stage);
  // Et il doit se DISTINGUER du repli même quand sa valeur vaut 25 °C : c'est
  // l'étage, pas la valeur, qui porte l'information.
  const TempCompDecision same = resolveCompensationTemp(NAN, kEzoFallbackTempC, 1, kStale);
  TEST_ASSERT_EQUAL_MESSAGE(TempCompStage::LastPlausible, same.stage,
      "Une derniere valeur egale a 25 reste l'etage 2, pas le repli.");
}

static void test_cascade_prefers_current_over_last_plausible(void) {
  const TempCompDecision d = resolveCompensationTemp(18.0f, 30.0f, 0, kStale);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, d.tempC);
  TEST_ASSERT_EQUAL(TempCompStage::Current, d.stage);
}

// --- D4.6 : l'invariant absolu — il y a TOUJOURS une valeur ---

static void test_cascade_never_returns_nan(void) {
  // ⛔ « Suspendre la compensation » n'existe pas : la cascade rend TOUJOURS un
  // nombre exploitable. Balayage croisé des cas dégénérés.
  const float probes[9] = {NAN, INFINITY, -INFINITY, kDs18b20PowerOnResetC,
                           kDeviceDisconnectedC, 1025.0f, -0.001f, 40.001f, 25.0f};
  const uint32_t ages[4] = {0, kStale, kStale + 1, 0xFFFFFFFFu};
  for (int m = 0; m < 9; ++m) {
    for (int l = 0; l < 9; ++l) {
      for (int a = 0; a < 4; ++a) {
        const TempCompDecision d =
            resolveCompensationTemp(probes[m], probes[l], ages[a], kStale);
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Cascade non numerique pour mesure=%g derniere=%g age=%u",
                 (double)probes[m], (double)probes[l], (unsigned)ages[a]);
        TEST_ASSERT_FALSE_MESSAGE(isnan(d.tempC), msg);
        TEST_ASSERT_TRUE_MESSAGE(isfinite(d.tempC), msg);
      }
    }
  }
}

static void test_cascade_output_is_always_a_plausible_temperature(void) {
  // Corollaire : la valeur qui part dans `RT` est toujours DANS la fenêtre.
  // C'est la garantie que la commande envoyée à l'EZO ne peut plus fausser le pH.
  const float probes[8] = {NAN, kDs18b20PowerOnResetC, kDeviceDisconnectedC,
                           1025.0f, -975.0f, 0.0f, 40.0f, 20.0f};
  const uint32_t ages[3] = {0, kStale, 0xFFFFFFFFu};
  for (int m = 0; m < 8; ++m) {
    for (int l = 0; l < 8; ++l) {
      for (int a = 0; a < 3; ++a) {
        const TempCompDecision d =
            resolveCompensationTemp(probes[m], probes[l], ages[a], kStale);
        char msg[176];
        snprintf(msg, sizeof(msg),
                 "RT,%g hors fenetre pour mesure=%g derniere=%g age=%u",
                 (double)d.tempC, (double)probes[m], (double)probes[l], (unsigned)ages[a]);
        TEST_ASSERT_TRUE_MESSAGE(isCompensationTempPlausible(d.tempC), msg);
      }
    }
  }
}

static void test_cascade_stale_window_is_the_sensor_stale_timeout(void) {
  // La fenêtre n'est pas une nouvelle constante : c'est celle qui sert déjà à
  // déclarer un capteur pH/ORP périmé (20 s = 10 lectures DS18B20 à 2 s).
  TEST_ASSERT_EQUAL_UINT32(20000u, kSensorStaleTimeoutMs);
}

static void test_cascade_stale_window_is_honoured_for_any_window(void) {
  // La fenêtre est un PARAMÈTRE : la cascade ne doit pas la court-circuiter au
  // profit d'une valeur codée en dur.
  const uint32_t windows[4] = {0u, 1000u, kStale, 120000u};
  for (int w = 0; w < 4; ++w) {
    const TempCompDecision inside =
        resolveCompensationTemp(NAN, 22.0f, windows[w], windows[w]);
    const TempCompDecision outside =
        resolveCompensationTemp(NAN, 22.0f, windows[w] + 1, windows[w]);
    char msg[128];
    snprintf(msg, sizeof(msg), "Fenetre %u ms non respectee", (unsigned)windows[w]);
    TEST_ASSERT_EQUAL_MESSAGE(TempCompStage::LastPlausible, inside.stage, msg);
    TEST_ASSERT_EQUAL_MESSAGE(TempCompStage::Fallback, outside.stage, msg);
  }
}

// =============================================================================
// D5 — Libellé d'étage ↔ étage rendu
// =============================================================================
// Ces libellés partent sur le topic MQTT RETENU `<base>/diagnostic`
// (`ezo_comp_stage`, src/mqtt_manager.cpp). Un décalage d'un cran rendrait le
// diagnostic TROMPEUR : on lirait « mesure » pendant un repli, c'est-à-dire
// exactement l'inverse de l'information qu'on est venu chercher.

static void test_stage_labels_are_the_documented_tokens(void) {
  TEST_ASSERT_EQUAL_STRING("mesure", tempCompStageLabel(TempCompStage::Current));
  TEST_ASSERT_EQUAL_STRING("derniere_valeur_plausible",
                           tempCompStageLabel(TempCompStage::LastPlausible));
  TEST_ASSERT_EQUAL_STRING("repli_defaut", tempCompStageLabel(TempCompStage::Fallback));
}

static void test_stage_labels_are_pairwise_distinct(void) {
  const char* a = tempCompStageLabel(TempCompStage::Current);
  const char* b = tempCompStageLabel(TempCompStage::LastPlausible);
  const char* c = tempCompStageLabel(TempCompStage::Fallback);
  TEST_ASSERT_TRUE(strcmp(a, b) != 0);
  TEST_ASSERT_TRUE(strcmp(b, c) != 0);
  TEST_ASSERT_TRUE(strcmp(a, c) != 0);
}

static void test_label_matches_the_stage_actually_resolved(void) {
  // On passe par la CASCADE, pas par l'enum : c'est l'appariement de bout en
  // bout (décision → libellé publié) qui est testé.
  const TempCompDecision cur  = resolveCompensationTemp(26.0f, NAN, 0, kStale);
  const TempCompDecision last = resolveCompensationTemp(NAN, 26.0f, 1000, kStale);
  const TempCompDecision fbk  = resolveCompensationTemp(NAN, NAN, 0, kStale);
  TEST_ASSERT_EQUAL_STRING("mesure", tempCompStageLabel(cur.stage));
  TEST_ASSERT_EQUAL_STRING("derniere_valeur_plausible", tempCompStageLabel(last.stage));
  TEST_ASSERT_EQUAL_STRING("repli_defaut", tempCompStageLabel(fbk.stage));
}

static void test_substitution_is_readable_from_the_label_alone(void) {
  // Un exploitant qui ne lit que `ezo_comp_stage` doit savoir s'il regarde une
  // mesure ou une substitution.
  const TempCompDecision cur = resolveCompensationTemp(26.0f, NAN, 0, kStale);
  TEST_ASSERT_TRUE(strcmp(tempCompStageLabel(cur.stage), "mesure") == 0);
  const float bad[3] = {NAN, kDs18b20PowerOnResetC, kDeviceDisconnectedC};
  for (int i = 0; i < 3; ++i) {
    const TempCompDecision sub = resolveCompensationTemp(bad[i], 26.0f, 1000, kStale);
    TEST_ASSERT_TRUE_MESSAGE(strcmp(tempCompStageLabel(sub.stage), "mesure") != 0,
        "Une substitution ne doit JAMAIS se presenter comme une 'mesure'.");
  }
}

static void test_stage_label_of_an_unknown_value_is_not_a_valid_stage(void) {
  // Défense : un enum corrompu ne doit pas se faire passer pour une mesure.
  const char* unknown = tempCompStageLabel(static_cast<TempCompStage>(200));
  TEST_ASSERT_EQUAL_STRING("inconnu", unknown);
  TEST_ASSERT_TRUE(strcmp(unknown, "mesure") != 0);
}

static void test_stage_enum_values_are_stable(void) {
  // L'entier part dans du diagnostic et dans des logs : le figer évite qu'une
  // réorganisation de l'enum rende les traces d'hier illisibles.
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(TempCompStage::Current));
  TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(TempCompStage::LastPlausible));
  TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(TempCompStage::Fallback));
}

// Helpers de lecture du dépôt, définis en D7 (sentinelles structurelles) et
// utilisés dès D6.3 : déclarés ici.
static char* readProjectFile(const char* rel, long* outLen);
static int countOccurrences(const char* hay, const char* needle);
static char* extractFunctionBody(const char* src, const char* sig);

// =============================================================================
// D6 — NON-RÉGRESSION exigée par le verdict chimique du 2026-09-07
// =============================================================================
// « On instrumente, on ne corrige pas. » Une sonde FIGÉE (feature-068) coûte
// ≈ 0,001 pH, deux à trois ordres de grandeur SOUS la bande morte la plus
// serrée (0,01). Trois invariants doivent donc être PROUVÉS, pas supposés.

// --- D6.1 : température figée ⇒ commande `RT` inchangée ---

static void test_frozen_temperature_yields_an_unchanged_rt_value(void) {
  // Une sonde figée rend la MÊME valeur plausible indéfiniment. La cascade n'a
  // aucune notion de « figé » : elle doit servir cette valeur telle quelle, à
  // l'identique, cycle après cycle. Si un jour la détection de gel se mettait à
  // substituer la température, ce test tomberait — et c'est exactement le but.
  const float frozenC = 26.94f;
  for (uint32_t cycle = 0; cycle < 400; ++cycle) {
    const uint32_t age = cycle * 5000u;  // 5 s par lecture EZO, jusqu'à ~33 min
    const TempCompDecision d = resolveCompensationTemp(frozenC, frozenC, age, kStale);
    char msg[128];
    snprintf(msg, sizeof(msg), "Cycle %u : RT a change alors que la sonde est figee",
             (unsigned)cycle);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(frozenC, d.tempC, msg);
    TEST_ASSERT_EQUAL_MESSAGE(TempCompStage::Current, d.stage, msg);
  }
}

static void test_frozen_temperature_is_not_corrected_at_any_plausible_value(void) {
  // Balayage de toute la fenêtre : aucune valeur plausible n'est « corrigée ».
  for (int i = 0; i <= 400; ++i) {
    const float t = (float)i * 0.1f;  // 0 … 40 °C
    const TempCompDecision d = resolveCompensationTemp(t, 25.0f, 0, kStale);
    char msg[128];
    snprintf(msg, sizeof(msg), "La mesure %g a ete alteree (rendu %g)",
             (double)t, (double)d.tempC);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(t, d.tempC, msg);
  }
}

static void test_frozen_temperature_ph_error_stays_negligible(void) {
  // Le chiffrage qui justifie « on n'agit pas » : une sonde figée à 26,94 alors
  // que l'eau a bougé de 1,5 °C coûte ≈ 0,001 pH.
  const float err = phErrorFromCompensation(7.2f, 28.4f, 26.94f);
  char msg[128];
  snprintf(msg, sizeof(msg), "Erreur pH d'une sonde figee = %g", (double)err);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(err) < 0.002f, msg);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(err) < 0.01f / 5.0f,
      "Une sonde figee doit rester tres en dessous de la bande morte 0,01.");
}

// --- D6.2 : le planning de filtration est inchangé ---

static void test_filtration_plan_is_identical_for_a_frozen_temperature(void) {
  // Le planning auto est déterministe en température : figée ⇒ même créneau,
  // recalcul après recalcul. Il est de plus protégé en amont par la bande morte
  // de 1 °C de filtration.cpp (une sonde figée ne franchit jamais 1 °C).
  const ScheduleWindow ref = computeAutoWindow(26.94f, kFiltrationPivotHour);
  for (int i = 0; i < 50; ++i) {
    const ScheduleWindow w = computeAutoWindow(26.94f, kFiltrationPivotHour);
    TEST_ASSERT_EQUAL_INT(ref.startMin, w.startMin);
    TEST_ASSERT_EQUAL_INT(ref.endMin, w.endMin);
  }
}

static void test_filtration_plan_is_immune_to_the_compensation_sentinels(void) {
  // Le planning est déjà immunisé et doit le rester : la durée est clampée à
  // [1 h ; 24 h] (src/schedule_logic.cpp), y compris pour les sentinelles du
  // capteur et pour le résultat d'un offset non borné.
  const float probes[6] = {kDeviceDisconnectedC, -55.0f, 0.0f,
                           kDs18b20PowerOnResetC, 125.0f, 1025.0f};
  for (int i = 0; i < 6; ++i) {
    const ScheduleWindow w = computeAutoWindow(probes[i], kFiltrationPivotHour);
    int duration = w.endMin - w.startMin;
    if (duration <= 0) duration += 1440;  // créneau qui franchit minuit
    char msg[144];
    snprintf(msg, sizeof(msg), "Temperature %g : duree de filtration %d min hors [60 ; 1440]",
             (double)probes[i], duration);
    TEST_ASSERT_TRUE_MESSAGE(duration >= 60 && duration <= 1440, msg);
    TEST_ASSERT_TRUE(w.startMin >= 0 && w.startMin < 1440);
    TEST_ASSERT_TRUE(w.endMin >= 0 && w.endMin < 1440);
  }
}

static void test_filtration_plan_still_follows_temperature_normally(void) {
  // Non-régression dans l'autre sens : le planning doit CONTINUER à réagir à la
  // température réelle. Une immunisation qui figerait le créneau serait une
  // régression, pas une protection.
  const ScheduleWindow cold = computeAutoWindow(14.0f, kFiltrationPivotHour);
  const ScheduleWindow warm = computeAutoWindow(28.0f, kFiltrationPivotHour);
  const int dCold = (cold.endMin - cold.startMin + 1440) % 1440;
  const int dWarm = (warm.endMin - warm.startMin + 1440) % 1440;
  TEST_ASSERT_TRUE_MESSAGE(dWarm > dCold,
      "Une eau plus chaude doit toujours donner un creneau plus long.");
  TEST_ASSERT_INT_WITHIN(2, 7 * 60, dCold);   // 14 °C / 2 = 7 h
  TEST_ASSERT_INT_WITHIN(2, 14 * 60, dWarm);  // 28 °C / 2 = 14 h
}

// --- D6.3 : aucune décision de dosage ne change ---

static void test_dosing_decisions_take_no_temperature_input(void) {
  // Preuve par CONSTRUCTION : `evaluateDose` ne reçoit que `DoseInputs`, qui ne
  // porte AUCUNE température (vérifié structurellement en D7.5). La compensation
  // ne peut donc pas déplacer une décision de dosage, quelle que soit la valeur
  // qui part dans `RT`. La sentinelle D7.4 verrouille l'autre moitié : aucun
  // chemin de pump_controller ne lit le drapeau « figé » ni l'étage.
  DoseInputs in;
  memset(&in, 0, sizeof(in));
  in.watchdogActive = true;
  in.waterPresent = true;
  in.reading = 7.40f;
  in.filterReady = true;
  in.filterState = DoseFilterState::Ready;
  in.filterUnstable = false;
  in.calPoints = 2;
  in.requiredPoints = 2;
  in.stabilizationActive = false;
  in.mixingActive = false;
  in.modeAutomatic = true;
  in.dailyInjectedMl = 10.0f;
  in.maxDailyMl = 300.0f;
  in.usedMs = 0;
  in.hourlyLimitMs = 600000UL;
  in.cyclesToday = 1;
  in.maxCyclesPerDay = 20;
  in.cyclesLastMin = 0;
  in.maxCyclesPerMin = 3;
  in.cyclesLast15Min = 0;
  in.maxCyclesPer15Min = 6;

  const DoseDecision ref = evaluateDose(in);
  TEST_ASSERT_TRUE_MESSAGE(ref.allowed,
      "Fixture de reference invalide : le dosage devrait etre autorise ici.");

  // Rejouée à l'identique : le verdict est déterministe et ne dépend d'aucun
  // état de température caché.
  for (int i = 0; i < 10; ++i) {
    const DoseDecision again = evaluateDose(in);
    TEST_ASSERT_EQUAL_MESSAGE(ref.allowed ? 1 : 0, again.allowed ? 1 : 0,
        "evaluateDose doit etre deterministe (aucune entree de temperature).");
    TEST_ASSERT_EQUAL(ref.cause, again.cause);
  }
}

static void test_dose_inputs_carry_no_temperature_field(void) {
  // Sentinelle de SURFACE : le jour où quelqu'un ajoutera un champ de
  // température à DoseInputs, l'invariant « aucune décision de dosage ne
  // change » cessera d'être vrai par construction — et il faudra repasser par
  // `pool-chemistry`. Ce test échoue AVANT.
  long len = 0;
  char* src = readProjectFile("src/dosing_logic.h", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/dosing_logic.h INTROUVABLE.");
  char* body = extractFunctionBody(src, "struct DoseInputs");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "struct DoseInputs INTROUVABLE (sentinelle aveugle).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "temp"),
      "DoseInputs porte desormais une temperature : l'invariant 'aucune decision "
      "de dosage ne change' n'est plus garanti par construction.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "Temp"),
      "DoseInputs porte desormais une temperature (cf. verdict pool-chemistry).");
  free(body);
  free(src);
}

// =============================================================================
// D7 — Sentinelles STRUCTURELLES
// =============================================================================
// Ce que la logique pure ne peut pas prouver seule : que la production BRANCHE
// bien cette logique, aux TROIS provenances de la valeur, et qu'elle ne branche
// PAS le drapeau « figé » sur une décision.

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_temp_compensation");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i == 0) {
      if (!rooted[0]) continue;
      snprintf(path, sizeof(path), "%s", rooted);
    } else if (i == 1) {
      snprintf(path, sizeof(path), "%s", rel);
    } else {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
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

// Nombre d'occurrences de `needle` dans `hay`.
static int countOccurrences(const char* hay, const char* needle) {
  int n = 0;
  const char* p = hay;
  const size_t len = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) { ++n; p += len; }
  return n;
}

// Extrait le corps { ... } de la fonction dont la signature commence à `sig`,
// par appariement d'accolades. Retourne un buffer alloué, ou NULL.
static char* extractFunctionBody(const char* src, const char* sig) {
  const char* p = strstr(src, sig);
  if (p == NULL) return NULL;
  const char* open = strchr(p, '{');
  if (open == NULL) return NULL;
  int depth = 0;
  const char* q = open;
  for (; *q != '\0'; ++q) {
    if (*q == '{') ++depth;
    else if (*q == '}') { --depth; if (depth == 0) { ++q; break; } }
  }
  const size_t n = (size_t)(q - open);
  char* body = (char*)malloc(n + 1);
  if (body == NULL) return NULL;
  memcpy(body, open, n);
  body[n] = '\0';
  return body;
}

// Retire EN PLACE les commentaires C et C++ de `s` (remplacés par des espaces,
// longueurs et numéros de ligne préservés). Sans cela, une sentinelle qui
// cherche `clampTempCalOffset(` est satisfaite par la simple MENTION du nom
// dans un commentaire — src/config.cpp:5 en contient une
// (« bug-114 : clampTempCalOffset() (logique pure) ») et la revue du 07/09/2026
// a DÉMONTRÉ qu'elle suffisait à garder le test vert après suppression de
// l'appel réel du chargement NVS. Un test qu'un commentaire satisfait est pire
// qu'un test absent : il produit une assertion verte qui atteste d'un invariant
// inexistant, et personne ne le rouvre.
static void stripCppComments(char* s) {
  bool inLine = false, inBlock = false, inStr = false, inChr = false;
  for (size_t i = 0; s[i] != '\0'; ++i) {
    if (inLine) {
      if (s[i] == '\n') inLine = false; else s[i] = ' ';
    } else if (inBlock) {
      if (s[i] == '*' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = false; }
      else if (s[i] != '\n') s[i] = ' ';
    } else if (inStr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '"') inStr = false;
    } else if (inChr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '\'') inChr = false;
    } else if (s[i] == '/' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inLine = true; }
    else if (s[i] == '/' && s[i + 1] == '*') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = true; }
    else if (s[i] == '"') inStr = true;
    else if (s[i] == '\'') inChr = true;
  }
}

// Lit un fichier du dépôt et en retire les commentaires. TOUTE sentinelle de
// câblage de ce fichier passe par ICI, jamais par le texte brut.
static char* readProjectCode(const char* rel) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src != NULL) stripCppComments(src);
  return src;
}

static void test_the_comment_stripper_actually_disarms_the_comment_loophole(void) {
  // Méta-test : il vérifie l'OUTIL des sentinelles. Sans lui, les assertions de
  // câblage qui suivent pourraient être vertes pour la mauvaise raison — c'est
  // exactement le défaut relevé en revue.
  char probe[] =
      "// bug-114 : clampTempCalOffset() (logique pure)\n"
      "/* clampTempCalOffset( en bloc */\n"
      "const char* s = \"clampTempCalOffset(\";\n"
      "x = clampTempCalOffset(y);\n";
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, countOccurrences(probe, "clampTempCalOffset("),
      "Le texte BRUT compte les 4 occurrences, dont 3 leurres.");
  stripCppComments(probe);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(probe, "clampTempCalOffset("),
      "Apres depouillement : le litteral de chaine et l'appel reel. Les "
      "commentaires LIGNE et BLOC doivent tous deux disparaitre.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(probe, "= clampTempCalOffset("),
      "Seule l'AFFECTATION reelle subsiste.");

  // Puis sur le fichier RÉEL qui portait le leurre.
  long len = 0;
  char* raw = readProjectFile("src/config.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(raw, "src/config.cpp INTROUVABLE.");
  const int rawCount = countOccurrences(raw, "clampTempCalOffset(");
  stripCppComments(raw);
  const int codeCount = countOccurrences(raw, "clampTempCalOffset(");
  char msg[256];
  snprintf(msg, sizeof(msg),
           "src/config.cpp : %d occurrence(s) brute(s) contre %d dans le code. Le "
           "depouillement doit en retirer au moins une (la mention de l'en-tete), "
           "sinon la sentinelle reste satisfiable par un commentaire.",
           rawCount, codeCount);
  TEST_ASSERT_TRUE_MESSAGE(rawCount > codeCount, msg);
  free(raw);
}

static void test_the_four_clamp_points_are_all_wired(void) {
  // QUATRE points de clamp — pas trois. Ils sont numérotés dans les commentaires
  // de production et se renvoient l'un à l'autre exprès : chacun couvre une
  // PROVENANCE différente de la valeur, aucun n'est un doublon.
  //   1/4 écriture        — saisie de l'opérateur (route /save-config) ;
  //   2/4 chargement NVS  — valeur HÉRITÉE d'une version sans borne, jamais
  //                         réenregistrée. C'est CE clamp qui garantit que
  //                         /get-config expose la valeur APPLIQUÉE ; sans lui le
  //                         piège documenté « config d'usine ≠ config vivante »
  //                         se rouvre (un diagnostic chiffré a déjà déraillé
  //                         dessus le 06/09/2026) ;
  //   3/4 émission mesure — _readDs18b20s() : valeur servie à /data, WS, MQTT,
  //                         à l'historique ET au planning de filtration ;
  //   4/4 émission RT     — getWaterTemperature() : le SEUL site qui alimente
  //                         réellement la commande `RT,<tempC>` de l'EZO pH.
  //
  // La sentinelle vise chaque point DANS SA FONCTION, sur du code DÉPOUILLÉ, et
  // exige les quatre NOMMÉMENT. Les deux versions faibles de cette assertion ont
  // été démontrées inopérantes en revue le 07/09/2026 : la première était
  // satisfaite par un commentaire de src/config.cpp:5, la seconde (« au moins un
  // appel dans le fichier ») laissait disparaître le site 4/4 — celui de `RT` —
  // sans rougir. Ne pas revenir à un compte au niveau du FICHIER.
  struct { const char* file; const char* sig; const char* label; } sites[4] = {
      {"src/web_routes_config.cpp", "static void handleSaveConfig(",
       "1/4 ecriture (route de configuration)"},
      {"src/config.cpp", "void loadMqttConfig(",
       "2/4 chargement NVS (garantit que /get-config dit la verite)"},
      {"src/sensors.cpp", "SensorManager::_readDs18b20s(",
       "3/4 emission mesure (/data, WS, MQTT, historique, planning)"},
      {"src/sensors.cpp", "SensorManager::getWaterTemperature() const",
       "4/4 emission RT (seul site alimentant la commande RT de l'EZO pH)"},
  };
  for (int i = 0; i < 4; ++i) {
    char* code = readProjectCode(sites[i].file);
    char msg[352];
    snprintf(msg, sizeof(msg), "%s INTROUVABLE (sentinelle aveugle)", sites[i].file);
    TEST_ASSERT_NOT_NULL_MESSAGE(code, msg);
    char* body = extractFunctionBody(code, sites[i].sig);
    snprintf(msg, sizeof(msg),
             "'%s' INTROUVABLE dans %s : la sentinelle est AVEUGLE. Si la fonction "
             "a ete renommee, mettre a jour cette table — ne PAS affaiblir "
             "l'assertion en comptant au niveau du fichier.",
             sites[i].sig, sites[i].file);
    TEST_ASSERT_NOT_NULL_MESSAGE(body, msg);
    snprintf(msg, sizeof(msg),
             "Point de clamp %s : aucun appel a clampTempCalOffset() dans le CODE de "
             "%s (%s). Les quatre points couvrent quatre PROVENANCES distinctes de "
             "l'offset : en supprimer un comme 'doublon' rouvre bug-114.",
             sites[i].label, sites[i].sig, sites[i].file);
    TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "clampTempCalOffset(") >= 1, msg);
    free(body);
    free(code);
  }
}

static void test_every_offset_write_path_is_clamped(void) {
  // Angle mort visé : une affectation à mqttCfg.tempCalibrationOffset qui
  // n'aurait PAS traversé le clamp. Comptes faits sur du code DÉPOUILLÉ, et sur
  // l'AFFECTATION `= clampTempCalOffset(` — pas sur la mention du nom.
  // Les LECTURES (/get-config, trame WS `config`, sauvegarde NVS) sont sûres par
  // transitivité : elles servent la valeur en mémoire, déjà écrêtée par 1/4 ou 2/4.
  const char* files[2] = {"src/web_routes_config.cpp", "src/config.cpp"};
  for (int i = 0; i < 2; ++i) {
    char* code = readProjectCode(files[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(code, "Fichier d'ecriture de l'offset INTROUVABLE.");
    const int writes = countOccurrences(code, "tempCalibrationOffset =");
    const int clamps = countOccurrences(code, "= clampTempCalOffset(");
    char msg[288];
    snprintf(msg, sizeof(msg),
             "%s : %d affectation(s) de tempCalibrationOffset pour %d affectation(s) "
             "depuis clampTempCalOffset() — un chemin d'ecriture echappe au clamp. "
             "(Comptes sur le CODE, commentaires depouilles.)",
             files[i], writes, clamps);
    TEST_ASSERT_TRUE_MESSAGE(writes >= 1, msg);
    TEST_ASSERT_TRUE_MESSAGE(clamps >= 1, msg);
    TEST_ASSERT_TRUE_MESSAGE(clamps >= writes, msg);
    free(code);
  }
}

static void test_both_emission_sites_feed_from_the_clamped_offset(void) {
  // Verrou NOMINATIF sur les deux sites d'émission de sensors.cpp, et verrou
  // d'EXHAUSTIVITÉ : dans chacune des deux fonctions, TOUTE lecture de
  // mqttCfg.tempCalibrationOffset doit être ENVELOPPÉE par clampTempCalOffset().
  // Une lecture nue rouvre le defaut chiffre (+1000 => `RT,1025.0` => +0,15 pH),
  // et le compte « >= 1 sur le fichier » ne la voyait pas.
  char* code = readProjectCode("src/sensors.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/sensors.cpp INTROUVABLE.");
  const char* sigs[2] = {"SensorManager::_readDs18b20s(",
                         "SensorManager::getWaterTemperature() const"};
  for (int i = 0; i < 2; ++i) {
    char* body = extractFunctionBody(code, sigs[i]);
    char msg[320];
    snprintf(msg, sizeof(msg), "'%s' INTROUVABLE : sentinelle aveugle.", sigs[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(body, msg);
    const int reads = countOccurrences(body, "mqttCfg.tempCalibrationOffset");
    const int wrapped =
        countOccurrences(body, "clampTempCalOffset(mqttCfg.tempCalibrationOffset)");
    snprintf(msg, sizeof(msg),
             "%s : %d lecture(s) de mqttCfg.tempCalibrationOffset pour %d "
             "enveloppee(s) par clampTempCalOffset(). Une lecture NUE de l'offset a "
             "l'emission rouvre bug-114 (offset +1000 => RT,1025.0 => +0,15 pH).",
             sigs[i], reads, wrapped);
    TEST_ASSERT_TRUE_MESSAGE(reads >= 1, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(reads, wrapped, msg);
    free(body);
  }
  free(code);
}

static void test_compensation_cascade_does_not_read_the_frozen_flag(void) {
  // « On instrumente, on ne corrige pas » : la résolution de la température de
  // compensation ne doit PAS consulter la détection de gel. Sinon une sonde
  // figée (≈ 0,001 pH) déclencherait un repli (≈ 0,01 pH) — dix fois pire, pour
  // un défaut dix fois plus petit.
  long len = 0;
  char* src = readProjectFile("src/sensors.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/sensors.cpp INTROUVABLE (sentinelle aveugle).");
  char* body = extractFunctionBody(src, "SensorManager::_resolveEzoCompensationTemp");
  TEST_ASSERT_NOT_NULL_MESSAGE(body,
      "_resolveEzoCompensationTemp INTROUVABLE : la sentinelle est aveugle, "
      "la renommer ici si la fonction a change de nom.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "Frozen"),
      "La cascade de compensation ne doit PAS lire la detection de gel "
      "(feature-068 : on instrumente, on ne corrige pas).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "frozen"),
      "La cascade de compensation ne doit PAS lire la detection de gel.");
  // Et elle DOIT bien passer par la logique pure testée ci-dessus.
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "resolveCompensationTemp(") >= 1,
      "La cascade de production doit deleguer a la logique pure resolveCompensationTemp().");
  free(body);
  free(src);
}

static void test_no_dosing_path_reads_the_frozen_flag_or_the_stage(void) {
  // L'autre moitié de l'invariant « aucune décision de dosage ne change » :
  // pump_controller.cpp ne doit consulter ni le drapeau « figé », ni l'étage de
  // compensation, ni la température de compensation. Le drapeau est INFORMATIF
  // et ne doit jamais inhiber quoi que ce soit.
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/pump_controller.cpp INTROUVABLE.");
  const char* forbidden[4] = {"isTemperatureFrozen", "getEzoCompensation",
                              "isEzoCompensationSubstituted", "TempCompStage"};
  for (int i = 0; i < 4; ++i) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "pump_controller.cpp reference '%s' : une decision de dosage depend "
             "desormais de la compensation de temperature. Verdict pool-chemistry "
             "du 07/09/2026 : ce drapeau est INFORMATIF (0,001 pH), il n'inhibe rien.",
             forbidden[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(src, forbidden[i]), msg);
  }
  free(src);
}

static void test_filtration_keeps_its_plan_when_temperature_is_invalid(void) {
  // L'immunité déjà en place côté filtration : NaN ⇒ on CONSERVE le planning
  // (pas de recalcul sur une valeur absente), et bande morte de 1 °C.
  long len = 0;
  char* src = readProjectFile("src/filtration.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/filtration.cpp INTROUVABLE.");
  char* body = extractFunctionBody(src, "FiltrationManager::computeAutoSchedule");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "computeAutoSchedule INTROUVABLE.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "isnan(referenceTemp)") >= 1,
      "Le planning auto doit CONSERVER son creneau quand la temperature est NaN.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "_lastScheduledTemp") >= 1,
      "La bande morte de 1 degre du planning auto a disparu.");
  // Et le planning ne doit pas se mettre à consommer l'étage de compensation.
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "getEzoCompensation"),
      "Le planning de filtration doit rester INCHANGE (aucun couplage a la compensation).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "Frozen"),
      "Le planning de filtration doit rester INCHANGE (aucun couplage au gel).");
  free(body);
  free(src);
}

// =============================================================================
int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();
  // D1 — clamp de l'offset
  RUN_TEST(test_clamp_offset_inside_range_is_identity);
  RUN_TEST(test_clamp_offset_at_exact_lower_bound_is_kept);
  RUN_TEST(test_clamp_offset_at_exact_upper_bound_is_kept);
  RUN_TEST(test_clamp_offset_just_inside_bounds_is_kept);
  RUN_TEST(test_clamp_offset_just_outside_bounds_is_clamped);
  RUN_TEST(test_clamp_offset_far_beyond_upper_is_clamped);
  RUN_TEST(test_clamp_offset_far_beyond_lower_is_clamped);
  RUN_TEST(test_clamp_offset_nan_is_neutralized_to_zero);
  RUN_TEST(test_clamp_offset_infinities_are_clamped);
  RUN_TEST(test_clamp_offset_is_idempotent);
  RUN_TEST(test_clamp_offset_output_is_always_within_bounds);
  RUN_TEST(test_offset_bounds_constants_are_symmetric_five_degrees);
  RUN_TEST(test_bounded_offset_keeps_ph_error_under_the_tightest_deadband);
  RUN_TEST(test_unbounded_positive_offset_would_cost_about_fifteen_hundredths_of_ph);
  RUN_TEST(test_unbounded_negative_offset_error_is_not_bounded);
  // D2 — isTempCalOffsetInRange
  RUN_TEST(test_offset_in_range_accepts_realistic_values);
  RUN_TEST(test_offset_in_range_at_exact_bounds_is_true);
  RUN_TEST(test_offset_out_of_range_just_outside_is_false);
  RUN_TEST(test_offset_in_range_rejects_nan_and_infinities);
  RUN_TEST(test_in_range_is_exactly_the_clamp_being_identity);
  // D3 — fenêtre de plausibilité
  RUN_TEST(test_plausible_accepts_the_whole_operating_range);
  RUN_TEST(test_plausible_at_exact_bounds_is_true);
  RUN_TEST(test_plausible_just_inside_bounds_is_true);
  RUN_TEST(test_implausible_just_outside_bounds_is_false);
  RUN_TEST(test_implausible_rejects_nan);
  RUN_TEST(test_the_nan_guard_of_the_plausibility_window_is_belt_and_braces);
  RUN_TEST(test_implausible_rejects_infinities);
  RUN_TEST(test_implausible_rejects_ds18b20_power_on_reset_sentinel);
  RUN_TEST(test_implausible_rejects_device_disconnected_sentinel);
  RUN_TEST(test_compensation_window_is_strictly_tighter_than_the_sensor_range);
  RUN_TEST(test_implausible_rejects_the_unbounded_offset_result);
  RUN_TEST(test_compensation_bounds_constants_are_zero_and_forty);
  RUN_TEST(test_plausibility_sweep_matches_the_closed_interval);
  // D4 — la cascade
  RUN_TEST(test_cascade_uses_the_current_measurement_when_plausible);
  RUN_TEST(test_cascade_current_ignores_last_plausible_and_its_age);
  RUN_TEST(test_cascade_current_at_exact_plausibility_bounds);
  RUN_TEST(test_cascade_falls_back_to_last_plausible_when_measure_is_nan);
  RUN_TEST(test_cascade_falls_back_to_last_plausible_for_each_sentinel);
  RUN_TEST(test_cascade_last_plausible_accepted_at_exact_stale_window);
  RUN_TEST(test_cascade_last_plausible_refused_just_past_stale_window);
  RUN_TEST(test_cascade_last_plausible_at_zero_age_is_fresh);
  RUN_TEST(test_cascade_last_plausible_must_itself_be_plausible);
  RUN_TEST(test_cascade_last_plausible_at_exact_bounds_is_served);
  RUN_TEST(test_cascade_chained_degradation_ends_on_fallback);
  RUN_TEST(test_cascade_fallback_when_nothing_is_available);
  RUN_TEST(test_cascade_fallback_value_is_twentyfive_degrees);
  RUN_TEST(test_cascade_fallback_is_plausible_by_construction);
  RUN_TEST(test_cascade_transition_current_then_last_then_fallback);
  RUN_TEST(test_cascade_transition_back_from_fallback_to_current);
  RUN_TEST(test_cascade_transition_last_plausible_back_to_current);
  RUN_TEST(test_cascade_prefers_last_plausible_over_fallback);
  RUN_TEST(test_cascade_prefers_current_over_last_plausible);
  RUN_TEST(test_cascade_never_returns_nan);
  RUN_TEST(test_cascade_output_is_always_a_plausible_temperature);
  RUN_TEST(test_cascade_stale_window_is_the_sensor_stale_timeout);
  RUN_TEST(test_cascade_stale_window_is_honoured_for_any_window);
  // D5 — libellés
  RUN_TEST(test_stage_labels_are_the_documented_tokens);
  RUN_TEST(test_stage_labels_are_pairwise_distinct);
  RUN_TEST(test_label_matches_the_stage_actually_resolved);
  RUN_TEST(test_substitution_is_readable_from_the_label_alone);
  RUN_TEST(test_stage_label_of_an_unknown_value_is_not_a_valid_stage);
  RUN_TEST(test_stage_enum_values_are_stable);
  // D6 — non-régression
  RUN_TEST(test_frozen_temperature_yields_an_unchanged_rt_value);
  RUN_TEST(test_frozen_temperature_is_not_corrected_at_any_plausible_value);
  RUN_TEST(test_frozen_temperature_ph_error_stays_negligible);
  RUN_TEST(test_filtration_plan_is_identical_for_a_frozen_temperature);
  RUN_TEST(test_filtration_plan_is_immune_to_the_compensation_sentinels);
  RUN_TEST(test_filtration_plan_still_follows_temperature_normally);
  RUN_TEST(test_dosing_decisions_take_no_temperature_input);
  RUN_TEST(test_dose_inputs_carry_no_temperature_field);
  // D7 — sentinelles structurelles
  RUN_TEST(test_the_comment_stripper_actually_disarms_the_comment_loophole);
  RUN_TEST(test_the_four_clamp_points_are_all_wired);
  RUN_TEST(test_every_offset_write_path_is_clamped);
  RUN_TEST(test_both_emission_sites_feed_from_the_clamped_offset);
  RUN_TEST(test_compensation_cascade_does_not_read_the_frozen_flag);
  RUN_TEST(test_no_dosing_path_reads_the_frozen_flag_or_the_stage);
  RUN_TEST(test_filtration_keeps_its_plan_when_temperature_is_invalid);
  return UNITY_END();
}
