// =============================================================================
// Tests unitaires natifs — Bascule de régulation en Manuel pendant un
// étalonnage (cal_manual_switch_logic, bug-160 volet 2, REFONTE 2026-10-13)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT OBSERVABLE de calSwitchShouldApplyEnter() /
// calSwitchShouldRestore() / calSwitchAlertDue() (src/cal_manual_switch_logic.h,
// ADR-0017) : remplace le mécanisme de bail cal_hold_logic (ABANDONNÉ) —
// gate pool-chemistry GATE 5, GO sous 8 conditions.
//
// CE QUE CETTE SUITE PROUVE, ET RIEN DE PLUS :
//   - calSwitchShouldApplyEnter() refuse de toucher une boucle déjà verrouillée
//     par une trace de sécurité (gate 5, C3) — tue le mutant « toucher au mode
//     pH alors que phAutoManual* est actif ».
//   - calSwitchShouldApplyEnter() refuse AUSSI de toucher une boucle DÉJÀ en
//     "manual" hors tout verrou de sécurité (bug-160, correctif 2026-10-13
//     v2) — tue le mutant « retirer `&& !alreadyManual` » : sans cette garde,
//     une entrée en calibration alors que l'utilisateur avait choisi "manual"
//     annoncerait à tort une bascule et armerait l'alerte d'oubli C4 pour un
//     état qu'il avait lui-même posé.
//   - calSwitchShouldRestore() ne restaure jamais une boucle non appliquée, et
//     le verrou de sécurité PRIME TOUJOURS sur la restauration même si la
//     session l'avait appliquée (escalade survenue PENDANT la calibration).
//   - calSwitchAlertDue() rend l'échéance EXACTE : 1re alerte à
//     kCalSwitchAlertFirstDelayS pile (jamais avant), répétée à
//     kCalSwitchAlertRepeatS pile (jamais avant) — tue le mutant « supprimer
//     l'échéance des 2 h ».
//
// CE QU'ELLE NE PROUVE PAS (périmètre honnête, à charge de la revue/CI —
// MÊME frontière que test_native_acct_window pour pump_controller.cpp) :
//   - « Mémorisation du mode ANTÉRIEUR, jamais "automatic" en dur » : le
//     stockage de mqttCfg.ph/orpCalSwitchPrevMode et son application à la
//     fermeture vivent dans web_routes_calibration.cpp, NON compilable
//     nativement (ESPAsyncWebServer, Arduino). Le mutant « restaurer
//     "automatic" en dur au lieu du mode mémorisé » n'est donc PAS tué par
//     cette suite — à vérifier en revue de code (code-reviewer) au site
//     `mqttCfg.phRegulationMode = mqttCfg.phCalSwitchPrevMode;` /
//     `mqttCfg.orpRegulationMode = mqttCfg.orpCalSwitchPrevMode;`.
//   - « N'acquitte JAMAIS phAutoManualEpoch/Reason à la fermeture » (gate 5,
//     C2, piège central) : structurellement garanti par l'ABSENCE de tout
//     paramètre/retour portant cette action dans ce module (cf. cal_manual_
//     switch_logic.h) — mais la garantie que la COQUILLE n'appelle pas
//     clearPhAutoManualTrace() sur ce chemin n'est, elle, vérifiable qu'en
//     lecture de web_routes_calibration.cpp (aucun appel de cette fonction
//     dans la route /calibration/exit).
//   - « Interrompt l'injection manuelle en cours à l'entrée » (gate 5, C6) :
//     vit dans web_routes_calibration.cpp (manualInjectPh/Orp.active, extern
//     depuis web_routes_control.cpp) — non compilable nativement.
//   - « Une voie basculée, l'autre non » (pH déjà manual, ORP automatic, ou
//     l'inverse) : calSwitchShouldApplyEnter() est appelée UNE FOIS PAR VOIE
//     avec SES PROPRES paramètres — ce fichier le prouve par construction
//     (deux appels indépendants, aucun état partagé), mais le câblage réel
//     `phAlreadyManual`/`orpAlreadyManual` (comparaison de
//     mqttCfg.ph/orpRegulationMode à "manual") vit dans
//     web_routes_calibration.cpp, non compilable nativement.
//   - bug-160 (correctif 2026-10-13 v2) « l'alerte C4 ne doit pas s'armer si
//     aucune voie n'a basculé » : c'est la coquille (web_routes_calibration.cpp)
//     qui décide de n'ouvrir la session (mqttCfg.calSwitchActive=true) QUE si
//     `applyPh || applyOrp` — ce module n'a ni accès ni connaissance de
//     calSwitchActive. Vérifiable en revue de code au site
//     `if (applyPh || applyOrp) { ... mqttCfg.calSwitchActive = true; ... }`.
// =============================================================================

#include <unity.h>
#include <stdint.h>

#include "cal_manual_switch_logic.h"
#include "constants.h"  // kCalSwitchAlertFirstDelayS / kCalSwitchAlertRepeatS

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// calSwitchShouldApplyEnter — gate 5, C3
// -----------------------------------------------------------------------------

void test_enter_pas_de_trace_securite_applique(void) {
  TEST_ASSERT_TRUE(calSwitchShouldApplyEnter(/*safetyTraceActive=*/false, /*alreadyManual=*/false));
}

// MUTATION À TUER : inverser le prédicat (ou le remplacer par `true` constant)
// ferait toucher le mode pH alors qu'il est DÉJÀ verrouillé par une escalade
// de sécurité réelle (feature-067, plancher acide) — le piège central du
// gate 5 (C2/C3).
void test_enter_trace_securite_active_ne_touche_pas(void) {
  TEST_ASSERT_FALSE(calSwitchShouldApplyEnter(/*safetyTraceActive=*/true, /*alreadyManual=*/true));
}

// Côté ORP (aucun verrou équivalent), le site d'appel passe TOUJOURS
// `safetyTraceActive=false` — documenté ici pour qu'une session future
// retrouve la garantie sans relire web_routes_calibration.cpp. Le mode
// courant est "automatic" (non "manual") : bascule normale.
void test_enter_orp_toujours_false_cote_appelant(void) {
  TEST_ASSERT_TRUE(calSwitchShouldApplyEnter(false, /*alreadyManual=*/false));
}

// -----------------------------------------------------------------------------
// calSwitchShouldApplyEnter — bug-160 correctif 2026-10-13 v2 : le mode
// courant COMPTE, pas seulement le verrou de sécurité.
// -----------------------------------------------------------------------------

// Déjà en "manual" (choix utilisateur, PAS un verrou de sécurité) : aucune
// bascule — c'est exactement le défaut rapporté par l'utilisateur (bandeau
// « mise en Manuel » + bouton « rétablir la régulation » alors que rien
// n'avait changé).
void test_enter_deja_manual_sans_verrou_ne_bascule_pas(void) {
  TEST_ASSERT_FALSE(calSwitchShouldApplyEnter(/*safetyTraceActive=*/false, /*alreadyManual=*/true));
}

// En "automatic" : bascule (cas nominal).
void test_enter_automatic_bascule(void) {
  TEST_ASSERT_TRUE(calSwitchShouldApplyEnter(/*safetyTraceActive=*/false, /*alreadyManual=*/false));
}

// En "scheduled" : bascule aussi — alreadyManual ne doit être vrai QUE pour
// "manual" exact, jamais pour "scheduled" (le site d'appel compare la String
// mode à "manual", pas une négation de "automatic").
void test_enter_scheduled_bascule(void) {
  TEST_ASSERT_TRUE(calSwitchShouldApplyEnter(/*safetyTraceActive=*/false, /*alreadyManual=*/false));
}

// Verrou de sécurité actif ET déjà en "manual" (cas réel : le verrou pH
// impose "manual") : aucune bascule, par les DEUX conditions à la fois.
void test_enter_verrou_et_deja_manual_ne_bascule_pas(void) {
  TEST_ASSERT_FALSE(calSwitchShouldApplyEnter(/*safetyTraceActive=*/true, /*alreadyManual=*/true));
}

// -----------------------------------------------------------------------------
// calSwitchShouldRestore — gate 5, C3 (le verrou de sécurité PRIME toujours)
// -----------------------------------------------------------------------------

void test_restore_non_applique_jamais_restaure(void) {
  TEST_ASSERT_FALSE(calSwitchShouldRestore(/*wasApplied=*/false, /*safetyTraceActiveNow=*/false));
  TEST_ASSERT_FALSE(calSwitchShouldRestore(/*wasApplied=*/false, /*safetyTraceActiveNow=*/true));
}

void test_restore_applique_et_aucun_verrou_restaure(void) {
  TEST_ASSERT_TRUE(calSwitchShouldRestore(/*wasApplied=*/true, /*safetyTraceActiveNow=*/false));
}

// MUTATION À TUER : retirer la garde `safetyTraceActiveNow` (ou l'ignorer)
// restaurerait le pH PAR-DESSUS une escalade de sécurité RÉELLE survenue
// PENDANT la session de calibration (l'opérateur calibre, le plancher acide
// escalade en silence, puis le bouton « Fermer » rouvrirait la régulation
// automatique sur une sonde que l'escalade venait de verrouiller — exactement
// le nouveau trou de sécurité que le gate 5 (C2) a explicitement identifié).
void test_restore_applique_mais_verrou_survenu_pendant_ne_restaure_pas(void) {
  TEST_ASSERT_FALSE(calSwitchShouldRestore(/*wasApplied=*/true, /*safetyTraceActiveNow=*/true));
}

// -----------------------------------------------------------------------------
// calSwitchAlertDue — gate 5, C4 (BLOQUANTE) : 1re alerte à 2 h pile, répétée
// à 6 h pile. Bornes testées EXACTEMENT sur les constantes de production
// (kCalSwitchAlertFirstDelayS / kCalSwitchAlertRepeatS), jamais un nombre en
// dur recopié à côté — une session future recontrôle, elle ne croit pas.
// -----------------------------------------------------------------------------

void test_alert_jamais_avant_la_premiere_echeance(void) {
  // 1 s avant l'échéance : MUET, quel que soit sinceLastAlertS (jamais encore
  // alertée, sentinelle UINT32_MAX).
  TEST_ASSERT_FALSE(calSwitchAlertDue(kCalSwitchAlertFirstDelayS - 1, UINT32_MAX,
                                      kCalSwitchAlertFirstDelayS, kCalSwitchAlertRepeatS));
  // Zéro seconde écoulée : MUET (cas trivial, mais c'est le point que le
  // mutant « supprimer l'échéance des 2 h » ferait passer au VERT à tort).
  TEST_ASSERT_FALSE(calSwitchAlertDue(0, UINT32_MAX,
                                      kCalSwitchAlertFirstDelayS, kCalSwitchAlertRepeatS));
}

// MUTATION À TUER : supprimer (ou contourner) la garde `elapsedS < firstDelayS`
// ferait sonner l'alerte dès l'entrée en calibration — un bruit immédiat sur
// CHAQUE étalonnage, pas un rappel d'oubli.
void test_alert_premiere_echeance_pile_declenche(void) {
  TEST_ASSERT_TRUE(calSwitchAlertDue(kCalSwitchAlertFirstDelayS, UINT32_MAX,
                                     kCalSwitchAlertFirstDelayS, kCalSwitchAlertRepeatS));
  // Bien après la première échéance, toujours « jamais encore alertée » :
  // doit rester déclenché (le compteur d'alerte n'a pas encore été posé).
  TEST_ASSERT_TRUE(calSwitchAlertDue(kCalSwitchAlertFirstDelayS + 3600UL, UINT32_MAX,
                                     kCalSwitchAlertFirstDelayS, kCalSwitchAlertRepeatS));
}

void test_alert_pas_de_repetition_avant_lecheance_de_repetition(void) {
  // Première alerte déjà émise (sinceLastAlertS fini) : MUET tant que
  // kCalSwitchAlertRepeatS n'est pas atteint, même très après la 1re échéance.
  TEST_ASSERT_FALSE(calSwitchAlertDue(kCalSwitchAlertFirstDelayS + 100000UL,
                                      kCalSwitchAlertRepeatS - 1,
                                      kCalSwitchAlertFirstDelayS, kCalSwitchAlertRepeatS));
}

void test_alert_repetition_pile_a_lecheance_declenche(void) {
  TEST_ASSERT_TRUE(calSwitchAlertDue(kCalSwitchAlertFirstDelayS + 100000UL,
                                     kCalSwitchAlertRepeatS,
                                     kCalSwitchAlertFirstDelayS, kCalSwitchAlertRepeatS));
}

// Séquence réaliste (coquille simulée tour par tour) : entrée, silence
// jusqu'à 2 h, alerte, silence jusqu'à 6 h de plus, alerte — jamais de double
// déclenchement entre deux échéances.
void test_alert_sequence_realiste_deux_h_puis_six_h(void) {
  const uint32_t first = kCalSwitchAlertFirstDelayS;
  const uint32_t repeat = kCalSwitchAlertRepeatS;

  // t = 0 : rien.
  TEST_ASSERT_FALSE(calSwitchAlertDue(0, UINT32_MAX, first, repeat));
  // t = first - 1 : rien.
  TEST_ASSERT_FALSE(calSwitchAlertDue(first - 1, UINT32_MAX, first, repeat));
  // t = first : 1re alerte.
  TEST_ASSERT_TRUE(calSwitchAlertDue(first, UINT32_MAX, first, repeat));
  // Juste après la 1re alerte (sinceLastAlertS redémarre à 0) : rien.
  TEST_ASSERT_FALSE(calSwitchAlertDue(first + 1, 0, first, repeat));
  // repeat - 1 après la 1re alerte : rien.
  TEST_ASSERT_FALSE(calSwitchAlertDue(first + repeat - 1, repeat - 1, first, repeat));
  // repeat pile après la 1re alerte : 2e alerte.
  TEST_ASSERT_TRUE(calSwitchAlertDue(first + repeat, repeat, first, repeat));
}

// =============================================================================
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_enter_pas_de_trace_securite_applique);
  RUN_TEST(test_enter_trace_securite_active_ne_touche_pas);
  RUN_TEST(test_enter_orp_toujours_false_cote_appelant);
  RUN_TEST(test_enter_deja_manual_sans_verrou_ne_bascule_pas);
  RUN_TEST(test_enter_automatic_bascule);
  RUN_TEST(test_enter_scheduled_bascule);
  RUN_TEST(test_enter_verrou_et_deja_manual_ne_bascule_pas);
  RUN_TEST(test_restore_non_applique_jamais_restaure);
  RUN_TEST(test_restore_applique_et_aucun_verrou_restaure);
  RUN_TEST(test_restore_applique_mais_verrou_survenu_pendant_ne_restaure_pas);
  RUN_TEST(test_alert_jamais_avant_la_premiere_echeance);
  RUN_TEST(test_alert_premiere_echeance_pile_declenche);
  RUN_TEST(test_alert_pas_de_repetition_avant_lecheance_de_repetition);
  RUN_TEST(test_alert_repetition_pile_a_lecheance_declenche);
  RUN_TEST(test_alert_sequence_realiste_deux_h_puis_six_h);
  return UNITY_END();
}
