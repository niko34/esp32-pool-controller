// =============================================================================
// Tests unitaires natifs — computeDoseAvailableInS (dose_available_logic.h)
// feature-139 (v2.62.0, B1 gate pool-chemistry 2e passage + code-reviewer 2e
// passage)
// =============================================================================
// TROIS défauts réels, trouvés successivement sur cette même formule :
//   1. (test-engineer, extraction) reste de purge non clampé à 0 une fois
//      measure_validity déjà valide (dispensé) — facteur 10 recréé.
//   2. (code-reviewer, 2e passage) même symptôme, TROISIÈME occurrence : eau
//      ABSENTE (filtration coupée) — aucun des deux planchers ne court,
//      figé à floorS (10 min) toute la nuit. Corrigé par la sentinelle
//      UINT32_MAX (`waterPresent` explicite, fonction TOTALE).
// Ces tests protègent la formule dans son état CORRIGÉ (signature à 5
// arguments, `waterPresent` en tête).
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "constants.h"
#include "dose_available_logic.h"

void setUp(void) {}
void tearDown(void) {}

static const uint32_t kFloorMs = kPurgeFloorMs;  // 10 min en production

// =============================================================================
// CAS 1 — Cycle DISPENSÉ, filtre encore gelé : le reste est celui du FILTRE
// (≈60 s), JAMAIS celui de la purge (10 min).
// =============================================================================
void test_cycle_dispense_filtre_gele_reste_est_celui_du_filtre(void) {
  // waterPresent = true, measuresValid = true (porte déjà ouverte par
  // dispense) ; stabElapsedS = 0 (measureValidityElapsedS() renvoie TOUJOURS 0
  // hors purge, dispense incluse) ; le plancher de gel du filtre vient d'être
  // réarmé (front montant d'eau), donc encore ~60 s pleines.
  const uint32_t result = computeDoseAvailableInS(/*waterPresent=*/true, /*measuresValid=*/true,
                                                  /*stabElapsedS=*/0UL, kFloorMs,
                                                  /*filterRemainingMs=*/60000UL);
  TEST_ASSERT_EQUAL_UINT32(60, result);  // PAS 600 (10 min) : c'est le défaut corrigé
  TEST_ASSERT_TRUE_MESSAGE(result < 600,
      "regression du facteur 10 : le reste de purge n'est pas clampe a 0 une fois dispense");

  // Le filtre s'écoule progressivement : le reste suit le filtre, jamais 600.
  const uint32_t mid = computeDoseAvailableInS(true, true, 0UL, kFloorMs, 30000UL);
  TEST_ASSERT_EQUAL_UINT32(30, mid);
  const uint32_t almostDone = computeDoseAvailableInS(true, true, 0UL, kFloorMs, 500UL);
  TEST_ASSERT_EQUAL_UINT32(1, almostDone);  // arrondi SUP : encore 1 s, pas 0
}

// =============================================================================
// CAS 2 — Cycle COMPLET (non dispensé) : c'est le plancher de PURGE qui domine
// =============================================================================
void test_cycle_complet_plancher_de_purge_domine(void) {
  // measuresValid = false : purge en cours, stabElapsedS progresse. Le
  // plancher de gel du filtre (60 s max) est armé au même front montant que
  // la purge (10 min) : il s'écoule bien avant elle, donc la purge domine
  // tout le reste du temps sauf tout début.
  const uint32_t debut = computeDoseAvailableInS(true, false, 0UL, kFloorMs, 60000UL);
  TEST_ASSERT_EQUAL_UINT32(600, debut);  // 10 min : la purge domine déjà (60 s < 600 s)

  const uint32_t filtreEcoule = computeDoseAvailableInS(true, false, 120UL, kFloorMs, 0UL);
  TEST_ASSERT_EQUAL_UINT32(480, filtreEcoule);

  const uint32_t presqueFini = computeDoseAvailableInS(true, false, 599UL, kFloorMs, 0UL);
  TEST_ASSERT_EQUAL_UINT32(1, presqueFini);
}

// =============================================================================
// CAS 3 — Les deux planchers écoulés : 0 PILE, jamais négatif ni repliement
// =============================================================================
void test_deux_planchers_ecoules_zero_jamais_negatif(void) {
  TEST_ASSERT_EQUAL_UINT32(0, computeDoseAvailableInS(true, true, 0UL, kFloorMs, 0UL));
  TEST_ASSERT_EQUAL_UINT32(0, computeDoseAvailableInS(true, false, 600UL, kFloorMs, 0UL));
  TEST_ASSERT_EQUAL_UINT32(0, computeDoseAvailableInS(true, false, 999999UL, kFloorMs, 0UL));
  TEST_ASSERT_EQUAL_UINT32(0, computeDoseAvailableInS(true, true, 999999UL, kFloorMs, 0UL));
}

// =============================================================================
// CAS 4 — Arrondi ms → s PAR EXCÈS : jamais « 0 s » tant qu'il reste du temps
// =============================================================================
void test_arrondi_par_exces_jamais_zero_tant_qu_il_reste_du_temps(void) {
  TEST_ASSERT_EQUAL_UINT32(1, computeDoseAvailableInS(true, true, 0UL, kFloorMs, 1UL));
  TEST_ASSERT_EQUAL_UINT32(1, computeDoseAvailableInS(true, true, 0UL, kFloorMs, 999UL));
  TEST_ASSERT_EQUAL_UINT32(1, computeDoseAvailableInS(true, true, 0UL, kFloorMs, 1000UL));  // 1000 ms = 1 s PILE
  TEST_ASSERT_EQUAL_UINT32(2, computeDoseAvailableInS(true, true, 0UL, kFloorMs, 1001UL));  // s suivante
  TEST_ASSERT_EQUAL_UINT32(0, computeDoseAvailableInS(true, true, 0UL, kFloorMs, 0UL));  // 0 ms : 0 s, licite
}

// =============================================================================
// CAS 5 — Borne STRUCTURELLE : kPurgeFloorMs jusqu'à 30 min (static_assert
// constants.h), pas seulement la valeur courante (10 min)
// =============================================================================
void test_borne_structurelle_plancher_jusqu_a_30_min(void) {
  const uint32_t floor30min = 1800000UL;  // borne haute assertée dans constants.h
  TEST_ASSERT_EQUAL_UINT32(1800, computeDoseAvailableInS(true, false, 0UL, floor30min, 0UL));
  TEST_ASSERT_EQUAL_UINT32(1, computeDoseAvailableInS(true, false, 1799UL, floor30min, 0UL));
  const uint32_t floor5min = 300000UL;
  TEST_ASSERT_EQUAL_UINT32(300, computeDoseAvailableInS(true, false, 0UL, floor5min, 0UL));
}

// =============================================================================
// CAS 6 — Eau ABSENTE (filtration coupée) : UINT32_MAX, JAMAIS floorS ni 0
// =============================================================================
// Défaut corrigé (code-reviewer, 2e passage) : filtration coupée toute la
// nuit, code 22 émis en permanence, measureValidityElapsedS() figé à 0 ⇒
// l'ancien calcul retombait sur floorS (600 s, 10 min) et restait FIGÉ,
// jamais décompté. La fonction doit désormais renvoyer la sentinelle
// UINT32_MAX, QUELLES QUE SOIENT les autres entrées (measuresValid,
// stabElapsedS, filterRemainingMs) — l'eau absente prime sur tout.
void test_eau_absente_sentinelle_uint32_max_jamais_floor_ni_zero(void) {
  const uint32_t a = computeDoseAvailableInS(/*waterPresent=*/false, false, 0UL, kFloorMs, 60000UL);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, a);
  TEST_ASSERT_NOT_EQUAL(600, a);
  TEST_ASSERT_NOT_EQUAL(0, a);

  // Même verdict quelle que soit la combinaison des autres paramètres —
  // l'absence d'eau prime INCONDITIONNELLEMENT (fonction TOTALE).
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, computeDoseAvailableInS(false, true, 0UL, kFloorMs, 0UL));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, computeDoseAvailableInS(false, false, 999999UL, kFloorMs, 0UL));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, computeDoseAvailableInS(false, true, 0UL, 1800000UL, 999999UL));
}

// Eau absente puis eau présente : la valeur redevient un nombre FINI cohérent
// (la sentinelle ne « colle » pas une fois l'eau revenue).
void test_eau_absente_puis_presente_redevient_un_nombre_fini(void) {
  const uint32_t pendantAbsence = computeDoseAvailableInS(false, false, 0UL, kFloorMs, 0UL);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, pendantAbsence);

  // Eau de retour, purge qui redémarre tout juste (ni measuresValid, ni
  // stabElapsedS écoulé, filtre réarmé) : valeur finie, cohérente avec le
  // cas 2 (purge qui domine).
  const uint32_t apresRetour = computeDoseAvailableInS(true, false, 0UL, kFloorMs, 60000UL);
  TEST_ASSERT_EQUAL_UINT32(600, apresRetour);
  TEST_ASSERT_NOT_EQUAL(UINT32_MAX, apresRetour);
}

// UINT32_MAX (indéterminé) doit rester DISCERNABLE d'un 0 légitime (fin
// d'attente) — c'est la distinction dont dépend tout l'affichage (la coquille
// OMET la clé JSON sur UINT32_MAX, mais publie 0 tel quel).
void test_uint32_max_discernable_d_un_zero_legitime(void) {
  const uint32_t zeroLegitime = computeDoseAvailableInS(true, true, 0UL, kFloorMs, 0UL);
  const uint32_t indetermine = computeDoseAvailableInS(false, true, 0UL, kFloorMs, 0UL);
  TEST_ASSERT_EQUAL_UINT32(0, zeroLegitime);
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, indetermine);
  TEST_ASSERT_TRUE_MESSAGE(zeroLegitime != indetermine,
      "0 (dosage deja possible) et UINT32_MAX (indetermine) doivent rester distincts");
}

// =============================================================================
// Runner
// =============================================================================
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cycle_dispense_filtre_gele_reste_est_celui_du_filtre);
  RUN_TEST(test_cycle_complet_plancher_de_purge_domine);
  RUN_TEST(test_deux_planchers_ecoules_zero_jamais_negatif);
  RUN_TEST(test_arrondi_par_exces_jamais_zero_tant_qu_il_reste_du_temps);
  RUN_TEST(test_borne_structurelle_plancher_jusqu_a_30_min);
  RUN_TEST(test_eau_absente_sentinelle_uint32_max_jamais_floor_ni_zero);
  RUN_TEST(test_eau_absente_puis_presente_redevient_un_nombre_fini);
  RUN_TEST(test_uint32_max_discernable_d_un_zero_legitime);
  return UNITY_END();
}
