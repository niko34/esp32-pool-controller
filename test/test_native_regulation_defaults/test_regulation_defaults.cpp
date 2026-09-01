// =============================================================================
// Tests unitaires natifs — Valeurs d'usine de régulation (feature-088, v2.31.0)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable du header pur src/regulation_defaults.h
// (ADR-0017), header-only : aucune entrée build_src_filter requise.
//
// Trois familles :
//
//   D1 — applyRegulationDefaults<Cfg>() : les 8 champs de tuning sont copiés
//        (valeurs EXACTES du recalibrage v2.30.0, visa pool-chemistry du
//        31/08/2026 — un recalibrage futur DOIT consciemment retoucher ces
//        tests), idempotence, alignement de kDefaultPhTarget sur la grille
//        kPhTargetStep (non prouvable en static_assert : arithmétique float).
//
//   D2 — INVARIANT CENTRAL (condition 3 du visa) : AUCUN champ hors périmètre
//        n'est touché. Le template est instancié sur un DOUBLE de MqttConfig
//        portant TOUS les champs sensibles avec des valeurs SENTINELLES
//        (choisies différentes des valeurs d'usine plausibles : un mutant qui
//        ajouterait `c.phCorrectionType = "ph_minus"` doit mourir, d'où la
//        sentinelle "ph_plus", etc.). Après application : sentinelles intactes,
//        comparées UNE À UNE.
//
//   D3 — regulationDefaultsRaiseDosing() : niveau de journal (condition 1 du
//        visa). Vrai ssi la restauration AUGMENTE une capacité de dosage
//        (consigne ORP qui monte, limite horaire pH/ORP qui monte) — les deux
//        sens et les frontières d'ÉGALITÉ (strictement supérieur : x → x
//        n'augmente rien).
//
// Familles ajoutées par la feature-090 (v2.32.0, table par taille de bassin) :
//
//   D4 — Colonnes Small et Large : les 8 champs prennent les valeurs de la
//        colonne, écrites EN DUR (littéraux de la spec, PAS des lectures de
//        kRegulationDefaultsBySize : un mutant qui altère une colonne doit
//        mourir ici) ; sentinelles hors périmètre intactes pour les TROIS
//        tailles ; consigne pH sur la grille kPhTargetStep pour les trois
//        colonnes (float, non prouvable en static_assert).
//
//   D5 — Sérialisation PoolSize : "small"/"medium"/"large" ↔ enum, round-trip,
//        et FAIL-SAFE de migration : nullptr, chaîne vide, casse différente ou
//        NVS corrompue ⇒ Medium.
//
//   D6 — MIGRATION (le test central du cycle) : une valeur pool_size inconnue
//        en NVS aboutit aux défauts v2.30.0 EXACTS, littéraux EN DUR (pas via
//        les kDefault*, qui suivraient une dérive de la table) ; et
//        regulationDefaultsRaiseDosing paramétré par les transitions de taille
//        (Small→Medium, Medium→Small, Medium→Large, appareil de référence).
// =============================================================================

#include <unity.h>
#include <math.h>    // roundf (grille kPhTargetStep)
#include <string.h>  // strcmp / strncpy

#include "regulation_defaults.h"  // module sous test (tire constants.h)

// feature-090 : applyRegulationDefaults() et regulationDefaultsRaiseDosing()
// sont désormais paramétrés par la COLONNE de taille de bassin
// (kRegulationDefaultsBySize). Ces tests historiques (feature-088) portent sur
// la colonne MEDIUM — celle dont les kDefault* restent les alias (verrou de
// migration v2.30.0). Les tests des autres colonnes (bornes, monotonie,
// sérialisation PoolSize) sont ajoutés par le test-engineer de la feature-090.
static const RegulationDefaults& kMediumDefaults = regulationDefaultsFor(PoolSize::Medium);

void setUp(void) {}
void tearDown(void) {}

// Mini-chaîne à VALEUR pour le double de config : libc++ (<string>) est
// indisponible sur l'hôte (cf. note de test_ph_offset.cpp) et la String du
// shim logger est un stub sans stockage. Suffit pour affecter (comme le fait
// applyRegulationDefaults sur mqttCfg.regulationSpeed) et comparer.
struct MiniStr {
  char buf[40];
  MiniStr(const char* s) { assign(s); }              // NOLINT implicite voulu
  MiniStr& operator=(const char* s) { assign(s); return *this; }
  const char* c_str() const { return buf; }
 private:
  void assign(const char* s) {
    strncpy(buf, s ? s : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  }
};

// ---------------------------------------------------------------------------
// DOUBLE de MqttConfig (config.h dépend d'Arduino.h → hors d'atteinte en
// natif). Porte les 8 champs du périmètre + TOUS les champs sensibles listés
// par le visa pool-chemistry, avec des SENTINELLES hors valeurs d'usine.
// ---------------------------------------------------------------------------
struct TestCfg {
  // --- Périmètre : les 8 champs de tuning restaurables --------------------
  // Initialisés TOUS différents des défauts (pré-condition des tests D1).
  float phTarget = 7.45f;               // ≠ 7.3
  float orpTarget = 650.0f;             // ≠ 600
  int phInjectionLimitMinutes = 25;     // ≠ 10
  int orpInjectionLimitMinutes = 5;     // ≠ 10
  int stabilizationDelayMin = 12;       // ≠ 5
  int phDailyTargetMl = 300;            // ≠ 0
  int orpDailyTargetMl = 450;           // ≠ 0
  MiniStr regulationSpeed = "fast"; // ≠ "normal"

  // --- HORS périmètre : sentinelles (visa : JAMAIS touchés) ----------------
  // Type de produit / modes / activation — sentinelles ≠ défauts d'usine de
  // MqttConfig ("ph_minus", "automatic", false) pour tuer un mutant qui
  // « restaurerait » aussi ces champs.
  MiniStr phCorrectionType = "ph_plus";
  MiniStr phRegulationMode = "scheduled";
  MiniStr orpRegulationMode = "manual";
  bool phEnabled = true;
  bool orpEnabled = true;
  // Trace de bascule auto→manuel (leçon phAutoManual).
  uint32_t phAutoManualEpoch = 1756600000u;
  MiniStr phAutoManualReason = "sonde pH divergente";
  // Offset de mesure pH (feature-067) — ≠ 0.0 (défaut usine).
  float phOffset = 0.42f;
  // Calibration / caractérisation des pompes (état PHYSIQUE).
  uint8_t pump1CalibratedDuty = 111;
  uint8_t pump2CalibratedDuty = 222;
  float pump1PulseFlowMlPerMin = 3.7f;
  float pump2PulseFlowMlPerMin = 8.1f;
  bool pump1FlowWeighed = true;
  bool pump2FlowWeighed = true;
  uint8_t pump1MaxDutyPct = 37;
  uint8_t pump2MaxDutyPct = 73;
  // Sélection des pompes — inversée par rapport au défaut (ph=1, orp=2).
  int phPump = 2;
  int orpPump = 1;
  // Mode d'installation (enum côté firmware ; un int suffit pour la copie).
  int installMode = 42;
  // Champs réseau (aucun rapport avec le tuning).
  MiniStr server = "10.0.0.99";
  int port = 1884;
  MiniStr topic = "piscine/test";
};

// ---------------------------------------------------------------------------
// D1 — Copie des 8 champs de tuning
// ---------------------------------------------------------------------------

// Les 8 défauts sont copiés, aux valeurs EXACTES du recalibrage v2.30.0.
// Toutes les valeurs de départ diffèrent des défauts (cf. TestCfg) : un mutant
// qui RETIRE un champ de la copie (ex. orpTarget) meurt ici.
void test_apply_copies_all_eight_defaults(void) {
  TestCfg c;
  applyRegulationDefaults(c, PoolSize::Medium);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.3f, c.phTarget);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 600.0f, c.orpTarget);
  TEST_ASSERT_EQUAL_INT(10, c.phInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(10, c.orpInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(5, c.stabilizationDelayMin);
  TEST_ASSERT_EQUAL_INT(0, c.phDailyTargetMl);
  TEST_ASSERT_EQUAL_INT(0, c.orpDailyTargetMl);
  TEST_ASSERT_EQUAL_STRING("normal", c.regulationSpeed.c_str());
}

// Cohérence copie ↔ constantes : le template écrit BIEN les k* du header
// (pas des littéraux dupliqués qui divergeraient au prochain recalibrage).
void test_apply_writes_the_named_constants(void) {
  TestCfg c;
  applyRegulationDefaults(c, PoolSize::Medium);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kDefaultPhTarget, c.phTarget);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, kDefaultOrpTarget, c.orpTarget);
  TEST_ASSERT_EQUAL_INT(kDefaultPhInjectionLimitMin, c.phInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(kDefaultOrpInjectionLimitMin, c.orpInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(kDefaultStabilizationDelayMin, c.stabilizationDelayMin);
  TEST_ASSERT_EQUAL_INT(kDefaultPhDailyTargetMl, c.phDailyTargetMl);
  TEST_ASSERT_EQUAL_INT(kDefaultOrpDailyTargetMl, c.orpDailyTargetMl);
  TEST_ASSERT_EQUAL_STRING(kDefaultRegulationSpeed, c.regulationSpeed.c_str());
}

// Idempotence : appliquer deux fois = même résultat (champ à champ, y compris
// les sentinelles hors périmètre).
void test_apply_is_idempotent(void) {
  TestCfg once;
  applyRegulationDefaults(once, PoolSize::Medium);
  TestCfg twice;
  applyRegulationDefaults(twice, PoolSize::Medium);
  applyRegulationDefaults(twice, PoolSize::Medium);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, once.phTarget, twice.phTarget);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, once.orpTarget, twice.orpTarget);
  TEST_ASSERT_EQUAL_INT(once.phInjectionLimitMinutes, twice.phInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(once.orpInjectionLimitMinutes, twice.orpInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(once.stabilizationDelayMin, twice.stabilizationDelayMin);
  TEST_ASSERT_EQUAL_INT(once.phDailyTargetMl, twice.phDailyTargetMl);
  TEST_ASSERT_EQUAL_INT(once.orpDailyTargetMl, twice.orpDailyTargetMl);
  TEST_ASSERT_EQUAL_STRING(once.regulationSpeed.c_str(), twice.regulationSpeed.c_str());
  TEST_ASSERT_EQUAL_STRING(once.phCorrectionType.c_str(), twice.phCorrectionType.c_str());
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, once.phOffset, twice.phOffset);
}

// kDefaultPhTarget est sur la grille UI [kPhTargetMin ; kPhTargetMax] de pas
// kPhTargetStep (0,1) : sinon le slider de la page pH ne pourrait jamais
// re-sélectionner exactement la valeur d'usine. Non prouvable en static_assert
// (arithmétique float non constexpr-exacte) — prouvé ici.
void test_default_ph_target_is_on_the_ui_step_grid(void) {
  const float steps = (kDefaultPhTarget - kPhTargetMin) / kPhTargetStep;
  const float snapped = kPhTargetMin + roundf(steps) * kPhTargetStep;
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, snapped, kDefaultPhTarget);
}

// La vitesse d'usine est l'un des trois libellés compris par le firmware/UI.
void test_default_regulation_speed_is_a_known_label(void) {
  const bool known = (strcmp(kDefaultRegulationSpeed, "slow") == 0) ||
                     (strcmp(kDefaultRegulationSpeed, "normal") == 0) ||
                     (strcmp(kDefaultRegulationSpeed, "fast") == 0);
  TEST_ASSERT_TRUE_MESSAGE(known, "kDefaultRegulationSpeed doit etre slow/normal/fast");
}

// ---------------------------------------------------------------------------
// D2 — INVARIANT : aucun champ hors périmètre n'est touché (condition 3)
// ---------------------------------------------------------------------------

// Chaque sentinelle est comparée UNE À UNE : un mutant qui AJOUTE un champ à
// la copie (ex. `c.phCorrectionType = "ph_minus"`) meurt ici. Helper partagé
// entre la colonne Medium (test historique 088) et les colonnes Small/Large
// (feature-090) : l'invariant « hors périmètre intouché » vaut PAR taille.
static void assertOutOfScopeSentinelsUntouched(const TestCfg& c) {
  TEST_ASSERT_EQUAL_STRING("ph_plus", c.phCorrectionType.c_str());
  TEST_ASSERT_EQUAL_STRING("scheduled", c.phRegulationMode.c_str());
  TEST_ASSERT_EQUAL_STRING("manual", c.orpRegulationMode.c_str());
  TEST_ASSERT_TRUE(c.phEnabled);
  TEST_ASSERT_TRUE(c.orpEnabled);

  TEST_ASSERT_EQUAL_UINT32(1756600000u, c.phAutoManualEpoch);
  TEST_ASSERT_EQUAL_STRING("sonde pH divergente", c.phAutoManualReason.c_str());
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.42f, c.phOffset);

  TEST_ASSERT_EQUAL_UINT8(111, c.pump1CalibratedDuty);
  TEST_ASSERT_EQUAL_UINT8(222, c.pump2CalibratedDuty);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.7f, c.pump1PulseFlowMlPerMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.1f, c.pump2PulseFlowMlPerMin);
  TEST_ASSERT_TRUE(c.pump1FlowWeighed);
  TEST_ASSERT_TRUE(c.pump2FlowWeighed);
  TEST_ASSERT_EQUAL_UINT8(37, c.pump1MaxDutyPct);
  TEST_ASSERT_EQUAL_UINT8(73, c.pump2MaxDutyPct);

  TEST_ASSERT_EQUAL_INT(2, c.phPump);
  TEST_ASSERT_EQUAL_INT(1, c.orpPump);
  TEST_ASSERT_EQUAL_INT(42, c.installMode);

  TEST_ASSERT_EQUAL_STRING("10.0.0.99", c.server.c_str());
  TEST_ASSERT_EQUAL_INT(1884, c.port);
  TEST_ASSERT_EQUAL_STRING("piscine/test", c.topic.c_str());
}

void test_apply_leaves_all_out_of_scope_fields_untouched(void) {
  TestCfg c;
  applyRegulationDefaults(c, PoolSize::Medium);
  assertOutOfScopeSentinelsUntouched(c);
}

// ---------------------------------------------------------------------------
// D3 — regulationDefaultsRaiseDosing (niveau de journal, condition 1)
// Sémantique : vrai ssi défaut STRICTEMENT > valeur courante pour AU MOINS
// une des trois capacités (orpTarget, limite pH, limite ORP).
// ---------------------------------------------------------------------------

// La consigne ORP MONTE (550 → 600 mV = plus de chlore visé), alors même que
// les limites DESCENDENT (25 → 10 min/h) : dosage augmenté ⇒ vrai.
void test_raise_dosing_true_when_orp_target_rises(void) {
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults, 550.0f, 25, 25));
}

// La limite pH MONTE (5 → 10 min/h, ex. bassin < 20 m³ redescendu à 5),
// tout le reste descend ou stagne ⇒ vrai.
void test_raise_dosing_true_when_ph_limit_rises(void) {
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults, 650.0f, 5, 25));
}

// La limite ORP MONTE seule ⇒ vrai.
void test_raise_dosing_true_when_orp_limit_rises(void) {
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults, 650.0f, 25, 5));
}

// Tout DESCEND (consigne ORP 650 → 600, limites 25 → 10) ⇒ faux : la
// restauration RÉDUIT la capacité de dosage, journal en info.
void test_raise_dosing_false_when_everything_descends(void) {
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(kMediumDefaults, 650.0f, 25, 25));
}

// Frontière : config courante EXACTEMENT aux valeurs d'usine — x → x
// n'augmente rien (comparaison STRICTE) ⇒ faux.
void test_raise_dosing_false_at_exact_equality(void) {
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(kMediumDefaults,
      kDefaultOrpTarget, kDefaultPhInjectionLimitMin, kDefaultOrpInjectionLimitMin));
}

// Frontières une à une : deux capacités à l'égalité exacte, la troisième
// juste EN DESSOUS du défaut ⇒ vrai (elle seule décide).
void test_raise_dosing_boundaries_one_field_at_a_time(void) {
  // orpTarget un chouia sous le défaut, limites à l'égalité.
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults,
      kDefaultOrpTarget - 1.0f, kDefaultPhInjectionLimitMin, kDefaultOrpInjectionLimitMin));
  // limite pH à défaut−1, le reste à l'égalité.
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults,
      kDefaultOrpTarget, kDefaultPhInjectionLimitMin - 1, kDefaultOrpInjectionLimitMin));
  // limite ORP à défaut−1, le reste à l'égalité.
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults,
      kDefaultOrpTarget, kDefaultPhInjectionLimitMin, kDefaultOrpInjectionLimitMin - 1));
  // Symétrique : chaque capacité à défaut+1 pendant que le reste est à
  // l'égalité ⇒ faux (tout descend ou stagne).
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(kMediumDefaults,
      kDefaultOrpTarget + 1.0f, kDefaultPhInjectionLimitMin, kDefaultOrpInjectionLimitMin));
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(kMediumDefaults,
      kDefaultOrpTarget, kDefaultPhInjectionLimitMin + 1, kDefaultOrpInjectionLimitMin));
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(kMediumDefaults,
      kDefaultOrpTarget, kDefaultPhInjectionLimitMin, kDefaultOrpInjectionLimitMin + 1));
}

// Le prédicat ne dépend PAS de la consigne pH ni des volumes programmés (remis
// à 0, jamais en hausse) : sa signature ne les prend même pas — on fige ici
// qu'il reste vrai/faux quel que soit le reste de la config (pas d'état caché).
void test_raise_dosing_is_stateless_across_calls(void) {
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults, 550.0f, 25, 25));
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(kMediumDefaults, 650.0f, 25, 25));
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(kMediumDefaults, 550.0f, 25, 25));
}

// ---------------------------------------------------------------------------
// D4 — Colonnes Small et Large (feature-090) : littéraux EN DUR
// ---------------------------------------------------------------------------
// Les valeurs attendues sont recopiées de la SPEC (visa pool-chemistry du
// 31/08/2026), PAS lues depuis kRegulationDefaultsBySize : comparer la table à
// elle-même ne tuerait aucun mutant. Toutes les valeurs de départ de TestCfg
// diffèrent des trois colonnes (cf. définition) : un champ non copié meurt.

// Small (< 20 m³) : {5, 5, "slow", 5, 7.3, 600, 0, 0}.
void test_apply_small_column_matches_spec_literals(void) {
  TestCfg c;
  applyRegulationDefaults(c, PoolSize::Small);
  TEST_ASSERT_EQUAL_INT(5, c.phInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(5, c.orpInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_STRING("slow", c.regulationSpeed.c_str());
  TEST_ASSERT_EQUAL_INT(5, c.stabilizationDelayMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.3f, c.phTarget);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 600.0f, c.orpTarget);
  TEST_ASSERT_EQUAL_INT(0, c.phDailyTargetMl);
  TEST_ASSERT_EQUAL_INT(0, c.orpDailyTargetMl);
}

// Large (> 60 m³) : {15, 15, "fast", 10, 7.3, 600, 0, 0}.
void test_apply_large_column_matches_spec_literals(void) {
  TestCfg c;
  applyRegulationDefaults(c, PoolSize::Large);
  TEST_ASSERT_EQUAL_INT(15, c.phInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(15, c.orpInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_STRING("fast", c.regulationSpeed.c_str());
  TEST_ASSERT_EQUAL_INT(10, c.stabilizationDelayMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.3f, c.phTarget);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 600.0f, c.orpTarget);
  TEST_ASSERT_EQUAL_INT(0, c.phDailyTargetMl);
  TEST_ASSERT_EQUAL_INT(0, c.orpDailyTargetMl);
}

// L'invariant « hors périmètre intouché » (condition 3 du visa) vaut pour les
// TROIS tailles : mêmes sentinelles, comparées une à une, après application de
// chaque colonne (le test historique couvre déjà Medium ; on re-couvre les
// trois pour qu'un mutant dépendant de la taille meure aussi).
void test_apply_leaves_out_of_scope_untouched_for_all_sizes(void) {
  const PoolSize sizes[3] = {PoolSize::Small, PoolSize::Medium, PoolSize::Large};
  for (int i = 0; i < 3; ++i) {
    TestCfg c;
    applyRegulationDefaults(c, sizes[i]);
    assertOutOfScopeSentinelsUntouched(c);
  }
}

// La consigne pH de CHAQUE colonne est sur la grille UI [kPhTargetMin ;
// kPhTargetMax] de pas kPhTargetStep — sinon le slider de la page pH ne
// pourrait jamais re-sélectionner la valeur d'usine de cette taille. Float :
// non prouvable en static_assert, prouvé ici (pendant multi-taille du test
// historique sur kDefaultPhTarget).
void test_ph_target_is_on_the_ui_step_grid_for_all_sizes(void) {
  const PoolSize sizes[3] = {PoolSize::Small, PoolSize::Medium, PoolSize::Large};
  for (int i = 0; i < 3; ++i) {
    const float target = regulationDefaultsFor(sizes[i]).phTarget;
    const float steps = (target - kPhTargetMin) / kPhTargetStep;
    const float snapped = kPhTargetMin + roundf(steps) * kPhTargetStep;
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, snapped, target);
  }
}

// ---------------------------------------------------------------------------
// D5 — Sérialisation PoolSize (clé NVS pool_size / JSON)
// ---------------------------------------------------------------------------

// Les trois libellés stables sont reconnus.
void test_pool_size_from_string_parses_the_three_valid_labels(void) {
  TEST_ASSERT_TRUE(poolSizeFromString("small") == PoolSize::Small);
  TEST_ASSERT_TRUE(poolSizeFromString("medium") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("large") == PoolSize::Large);
}

// Round-trip : toString(fromString(x)) == x pour les trois libellés valides
// (fige AUSSI poolSizeToString : "small"/"medium"/"large", jamais autre chose).
void test_pool_size_round_trip_for_valid_labels(void) {
  TEST_ASSERT_EQUAL_STRING("small", poolSizeToString(poolSizeFromString("small")));
  TEST_ASSERT_EQUAL_STRING("medium", poolSizeToString(poolSizeFromString("medium")));
  TEST_ASSERT_EQUAL_STRING("large", poolSizeToString(poolSizeFromString("large")));
}

// FAIL-SAFE de migration : tout ce qui n'est pas un libellé exact (nullptr,
// vide, casse différente, NVS corrompue, ancien format numérique) ⇒ Medium,
// la colonne v2.30.0 — un appareil existant ne change JAMAIS de comportement.
void test_pool_size_from_string_fail_safe_to_medium(void) {
  TEST_ASSERT_TRUE(poolSizeFromString(nullptr) == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("SMALL") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("Large") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("MEDIUM") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("xl") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("1") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("grand bassin") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString(" small") == PoolSize::Medium);
  TEST_ASSERT_TRUE(poolSizeFromString("small ") == PoolSize::Medium);
}

// ---------------------------------------------------------------------------
// D6 — MIGRATION (test central) et raiseDosing paramétré par les transitions
// ---------------------------------------------------------------------------

// LE test de migration : une valeur pool_size ILLISIBLE en NVS (appareil
// v2.31.0 sans la clé, flash corrompu, futur libellé inconnu) doit aboutir aux
// défauts v2.30.0 EXACTS. Littéraux EN DUR : passer par les kDefault* (alias
// de la colonne Medium) rendrait le test tautologique — il suivrait une
// dérive de la table au lieu de la détecter. Doublon assumé du static_assert
// de migration : si celui-ci est retiré ou affaibli, ce test reste le dernier
// filet.
void test_migration_unknown_pool_size_yields_v2_30_defaults(void) {
  TestCfg c;
  applyRegulationDefaults(c, poolSizeFromString("valeur NVS corrompue"));
  TEST_ASSERT_EQUAL_INT(10, c.phInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_INT(10, c.orpInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_STRING("normal", c.regulationSpeed.c_str());
  TEST_ASSERT_EQUAL_INT(5, c.stabilizationDelayMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.3f, c.phTarget);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 600.0f, c.orpTarget);
  TEST_ASSERT_EQUAL_INT(0, c.phDailyTargetMl);
  TEST_ASSERT_EQUAL_INT(0, c.orpDailyTargetMl);

  // Clé absente (nullptr) : même destination.
  TestCfg c2;
  applyRegulationDefaults(c2, poolSizeFromString(nullptr));
  TEST_ASSERT_EQUAL_INT(10, c2.phInjectionLimitMinutes);
  TEST_ASSERT_EQUAL_STRING("normal", c2.regulationSpeed.c_str());
}

// Transition Small → Medium : les limites MONTENT (5 → 10 min/h) alors que la
// consigne ORP stagne (600 → 600) ⇒ vrai (journal WARNING).
void test_raise_dosing_small_to_medium_is_true(void) {
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(
      regulationDefaultsFor(PoolSize::Medium), 600.0f, 5, 5));
}

// Transition Medium → Small : tout descend ou stagne (10 → 5, 600 → 600)
// ⇒ faux (journal info).
void test_raise_dosing_medium_to_small_is_false(void) {
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(
      regulationDefaultsFor(PoolSize::Small), 600.0f, 10, 10));
}

// Transition Medium → Large : les limites MONTENT (10 → 15 min/h) ⇒ vrai.
void test_raise_dosing_medium_to_large_is_true(void) {
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(
      regulationDefaultsFor(PoolSize::Large), 600.0f, 10, 10));
}

// Cas de l'appareil de RÉFÉRENCE (valeurs courantes réalistes, pas les
// colonnes) : limites relevées à la main à 25 min/h (alerte ADR-0056), consigne
// ORP abaissée à 550 mV. Passage à Large : les limites DESCENDENT (25 → 15)
// mais la consigne ORP MONTE (550 → 600) ⇒ vrai — une seule capacité en hausse
// suffit à exiger le WARNING.
void test_raise_dosing_reference_device_to_large_is_true(void) {
  TEST_ASSERT_TRUE(regulationDefaultsRaiseDosing(
      regulationDefaultsFor(PoolSize::Large), 550.0f, 25, 25));
}

// Même appareil mais consigne ORP courante 650 mV (> 600) : passage à Large,
// TOUT descend (650 → 600, 25 → 15) ⇒ faux malgré le changement de taille.
void test_raise_dosing_reference_device_to_large_all_descend_is_false(void) {
  TEST_ASSERT_FALSE(regulationDefaultsRaiseDosing(
      regulationDefaultsFor(PoolSize::Large), 650.0f, 25, 25));
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  // D1 — copie des 8 défauts
  RUN_TEST(test_apply_copies_all_eight_defaults);
  RUN_TEST(test_apply_writes_the_named_constants);
  RUN_TEST(test_apply_is_idempotent);
  RUN_TEST(test_default_ph_target_is_on_the_ui_step_grid);
  RUN_TEST(test_default_regulation_speed_is_a_known_label);

  // D2 — invariant : hors périmètre intouché
  RUN_TEST(test_apply_leaves_all_out_of_scope_fields_untouched);

  // D3 — prédicat du niveau de journal
  RUN_TEST(test_raise_dosing_true_when_orp_target_rises);
  RUN_TEST(test_raise_dosing_true_when_ph_limit_rises);
  RUN_TEST(test_raise_dosing_true_when_orp_limit_rises);
  RUN_TEST(test_raise_dosing_false_when_everything_descends);
  RUN_TEST(test_raise_dosing_false_at_exact_equality);
  RUN_TEST(test_raise_dosing_boundaries_one_field_at_a_time);
  RUN_TEST(test_raise_dosing_is_stateless_across_calls);

  // D4 — colonnes Small/Large (feature-090)
  RUN_TEST(test_apply_small_column_matches_spec_literals);
  RUN_TEST(test_apply_large_column_matches_spec_literals);
  RUN_TEST(test_apply_leaves_out_of_scope_untouched_for_all_sizes);
  RUN_TEST(test_ph_target_is_on_the_ui_step_grid_for_all_sizes);

  // D5 — sérialisation PoolSize
  RUN_TEST(test_pool_size_from_string_parses_the_three_valid_labels);
  RUN_TEST(test_pool_size_round_trip_for_valid_labels);
  RUN_TEST(test_pool_size_from_string_fail_safe_to_medium);

  // D6 — migration + transitions de taille
  RUN_TEST(test_migration_unknown_pool_size_yields_v2_30_defaults);
  RUN_TEST(test_raise_dosing_small_to_medium_is_true);
  RUN_TEST(test_raise_dosing_medium_to_small_is_false);
  RUN_TEST(test_raise_dosing_medium_to_large_is_true);
  RUN_TEST(test_raise_dosing_reference_device_to_large_is_true);
  RUN_TEST(test_raise_dosing_reference_device_to_large_all_descend_is_false);

  return UNITY_END();
}
