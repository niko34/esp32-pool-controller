// =============================================================================
// Tests unitaires natifs — Présence d'eau & migration mode d'installation
// (feature-056)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / I²C / FreeRTOS.
// On teste le COMPORTEMENT observable de la garde de sécurité chimique
// « présence d'eau » extraite dans src/dosing_logic.{h,cpp} :
//   - resolveWaterPresent(WaterPresenceInputs) → {waterPresent, source, stale}
//   - migrateInstallMode(regMode, filtrationEnabled) → InstallMode
//
// INVARIANT SÉCURITÉ (pool-chemistry, fail-closed strict) : au moindre doute
// (mode inconnu, signal externe jamais reçu, signal périmé, filtration non
// commandée) → waterPresent = false. Ces tests verrouillent ce contrat.
//
// Frontière de fraîcheur figée : age <= externalStaleMs = frais ; age > = périmé.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <math.h>  // C header uniquement (libc++ <cmath> indisponible sur l'hôte)
#include "dosing_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Valeur réelle projet (constants.h) — répliquée ici (module pur, pas d'include
// Arduino). Toute divergence firmware sera détectée par la revue.
static const uint32_t kStaleMs = 180000UL;  // kExternalFiltrationStaleMs (3 min)

// Helper : entrées External « signal ON, connu, frais » → eau présente.
static WaterPresenceInputs externalInputs(bool on, bool known, uint32_t ageMs) {
  WaterPresenceInputs in;
  in.mode = InstallMode::ExternalFiltration;
  in.filtrationCommandedOn = false;  // ignoré en mode External
  in.externalSignalOn = on;
  in.externalSignalKnown = known;
  in.externalSignalAgeMs = ageMs;
  in.externalStaleMs = kStaleMs;
  return in;
}

// =============================================================================
// Managed : eau présente ssi filtrationCommandedOn (source FiltrationCommanded)
// =============================================================================
void test_managed_water_present_when_commanded_on(void) {
  WaterPresenceInputs in;
  in.mode = InstallMode::ManagedFiltration;
  in.filtrationCommandedOn = true;
  in.externalSignalOn = false;       // ne doit pas influer
  in.externalSignalKnown = false;    // ne doit pas influer
  in.externalSignalAgeMs = 999999UL; // ne doit pas influer
  in.externalStaleMs = kStaleMs;
  WaterPresence r = resolveWaterPresent(in);
  TEST_ASSERT_TRUE(r.waterPresent);
  TEST_ASSERT_EQUAL(WaterSource::FiltrationCommanded, r.source);
  TEST_ASSERT_FALSE(r.stale);
}

void test_managed_water_absent_when_commanded_off(void) {
  WaterPresenceInputs in;
  in.mode = InstallMode::ManagedFiltration;
  in.filtrationCommandedOn = false;
  in.externalSignalOn = true;        // ne doit pas influer
  in.externalSignalKnown = true;
  in.externalSignalAgeMs = 0UL;
  in.externalStaleMs = kStaleMs;
  WaterPresence r = resolveWaterPresent(in);
  TEST_ASSERT_FALSE(r.waterPresent);
  TEST_ASSERT_EQUAL(WaterSource::FiltrationCommanded, r.source);
  TEST_ASSERT_FALSE(r.stale);
}

// =============================================================================
// PoweredByFiltration : eau TOUJOURS présente (présumée par câblage)
// =============================================================================
void test_powered_water_always_present(void) {
  // Même avec tous les autres champs « défavorables », l'eau est présumée.
  WaterPresenceInputs in;
  in.mode = InstallMode::PoweredByFiltration;
  in.filtrationCommandedOn = false;
  in.externalSignalOn = false;
  in.externalSignalKnown = false;
  in.externalSignalAgeMs = 999999UL;
  in.externalStaleMs = kStaleMs;
  WaterPresence r = resolveWaterPresent(in);
  TEST_ASSERT_TRUE(r.waterPresent);
  TEST_ASSERT_EQUAL(WaterSource::PoweredAssumed, r.source);
  TEST_ASSERT_FALSE(r.stale);
}

// =============================================================================
// ExternalFiltration : ON + connu + frais → présent
// =============================================================================
void test_external_on_known_fresh_present(void) {
  WaterPresence r = resolveWaterPresent(externalInputs(true, true, 1000UL));
  TEST_ASSERT_TRUE(r.waterPresent);
  TEST_ASSERT_EQUAL(WaterSource::ExternalSignal, r.source);
  TEST_ASSERT_FALSE(r.stale);
}

// ON + connu + périmé (age > stale) → absent, marqué stale.
void test_external_on_known_stale_absent(void) {
  WaterPresence r = resolveWaterPresent(externalInputs(true, true, kStaleMs + 1000UL));
  TEST_ASSERT_FALSE(r.waterPresent);
  TEST_ASSERT_EQUAL(WaterSource::ExternalSignal, r.source);
  TEST_ASSERT_TRUE(r.stale);  // connu mais trop ancien
}

// ON + jamais reçu (boot) → absent, PAS marqué stale (âge non signifiant).
void test_external_unknown_at_boot_absent(void) {
  // age « aberrant » : ne doit même pas être consulté (known testé AVANT l'âge).
  WaterPresence r = resolveWaterPresent(externalInputs(true, false, 0UL));
  TEST_ASSERT_FALSE(r.waterPresent);
  TEST_ASSERT_EQUAL(WaterSource::ExternalSignal, r.source);
  TEST_ASSERT_FALSE(r.stale);
}

// OFF + connu + frais → absent (signal explicite d'arrêt), non stale.
void test_external_off_known_fresh_absent(void) {
  WaterPresence r = resolveWaterPresent(externalInputs(false, true, 1000UL));
  TEST_ASSERT_FALSE(r.waterPresent);
  TEST_ASSERT_EQUAL(WaterSource::ExternalSignal, r.source);
  TEST_ASSERT_FALSE(r.stale);
}

// =============================================================================
// ExternalFiltration : frontières exactes de fraîcheur
// =============================================================================
// age == stale → encore frais (<= inclusif) → présent.
void test_external_boundary_age_equals_stale_present(void) {
  WaterPresence r = resolveWaterPresent(externalInputs(true, true, kStaleMs));
  TEST_ASSERT_TRUE(r.waterPresent);
  TEST_ASSERT_FALSE(r.stale);
}

// age == stale + 1 → périmé → absent, stale.
void test_external_boundary_age_stale_plus_one_absent(void) {
  WaterPresence r = resolveWaterPresent(externalInputs(true, true, kStaleMs + 1UL));
  TEST_ASSERT_FALSE(r.waterPresent);
  TEST_ASSERT_TRUE(r.stale);
}

// =============================================================================
// Mode inconnu / valeur enum hors plage → fail-closed (absent)
// =============================================================================
void test_unknown_mode_out_of_range_absent(void) {
  WaterPresenceInputs in;
  // Valeur hors des 3 valeurs valides (0/1/2) : simule une config corrompue.
  in.mode = static_cast<InstallMode>(99);
  in.filtrationCommandedOn = true;   // même « favorable » → doit rester refusé
  in.externalSignalOn = true;
  in.externalSignalKnown = true;
  in.externalSignalAgeMs = 0UL;
  in.externalStaleMs = kStaleMs;
  WaterPresence r = resolveWaterPresent(in);
  TEST_ASSERT_FALSE(r.waterPresent);
}

// =============================================================================
// migrateInstallMode : mapping ancien schéma → InstallMode (jamais External)
// =============================================================================
void test_migrate_continu_to_powered(void) {
  TEST_ASSERT_EQUAL(InstallMode::PoweredByFiltration,
                    migrateInstallMode("continu", true));
  // filtrationEnabled ne change rien pour "continu".
  TEST_ASSERT_EQUAL(InstallMode::PoweredByFiltration,
                    migrateInstallMode("continu", false));
}

void test_migrate_pilote_to_managed(void) {
  TEST_ASSERT_EQUAL(InstallMode::ManagedFiltration,
                    migrateInstallMode("pilote", true));
}

// "pilote" + filtration désactivée → Managed (absorption filtration_enabled).
void test_migrate_pilote_disabled_to_managed(void) {
  TEST_ASSERT_EQUAL(InstallMode::ManagedFiltration,
                    migrateInstallMode("pilote", false));
}

// Chaîne inconnue / nullptr → Managed (fallback sûr, jamais External).
void test_migrate_unknown_string_to_managed(void) {
  TEST_ASSERT_EQUAL(InstallMode::ManagedFiltration,
                    migrateInstallMode("n_importe_quoi", true));
  TEST_ASSERT_EQUAL(InstallMode::ManagedFiltration,
                    migrateInstallMode("", false));
  TEST_ASSERT_EQUAL(InstallMode::ManagedFiltration,
                    migrateInstallMode(nullptr, true));
}

// La migration ne produit JAMAIS ExternalFiltration, quelle que soit l'entrée.
void test_migrate_never_external(void) {
  const char* candidates[] = {"continu", "pilote", "external", "", "xyz"};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_NOT_EQUAL(InstallMode::ExternalFiltration,
                          migrateInstallMode(candidates[i], true));
    TEST_ASSERT_NOT_EQUAL(InstallMode::ExternalFiltration,
                          migrateInstallMode(candidates[i], false));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  // resolveWaterPresent — Managed
  RUN_TEST(test_managed_water_present_when_commanded_on);
  RUN_TEST(test_managed_water_absent_when_commanded_off);
  // resolveWaterPresent — Powered
  RUN_TEST(test_powered_water_always_present);
  // resolveWaterPresent — External
  RUN_TEST(test_external_on_known_fresh_present);
  RUN_TEST(test_external_on_known_stale_absent);
  RUN_TEST(test_external_unknown_at_boot_absent);
  RUN_TEST(test_external_off_known_fresh_absent);
  RUN_TEST(test_external_boundary_age_equals_stale_present);
  RUN_TEST(test_external_boundary_age_stale_plus_one_absent);
  // resolveWaterPresent — mode inconnu
  RUN_TEST(test_unknown_mode_out_of_range_absent);
  // migrateInstallMode
  RUN_TEST(test_migrate_continu_to_powered);
  RUN_TEST(test_migrate_pilote_to_managed);
  RUN_TEST(test_migrate_pilote_disabled_to_managed);
  RUN_TEST(test_migrate_unknown_string_to_managed);
  RUN_TEST(test_migrate_never_external);
  return UNITY_END();
}
