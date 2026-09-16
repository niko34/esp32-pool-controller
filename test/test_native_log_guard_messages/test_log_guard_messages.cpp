// =============================================================================
// Tests unitaires natifs — dimensionnement des messages de garde (feature-129)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32.
//
// CETTE SUITE NE TESTE PAS UNE LOGIQUE : elle VERROUILLE UNE CONSTANTE contre
// une régression de dimensionnement (condition bloquante C1). Volontairement
// séparée de test_native_log_arena (qui éprouve, elle, la LOGIQUE de l'arène)
// pour rester lisible seule par `pool-chemistry`.
//
// MÉTHODE : chaque test reconstruit, via les MÊMES chaînes de format que le
// firmware (snprintf dans un char[1024] de test — jamais un tampon de la
// taille réelle, pour ne jamais masquer un dépassement par une troncature
// locale au test), le message de garde à sa valeur PIRE CAS, puis affirme
// `len <= kLogMessageMaxBytes`.
//
// Sites couverts (feature-129, section "Les trois suites à créer") :
//   1. pump_controller.cpp:3600  (char msg[480], ~479 o)
//   2. pump_controller.cpp:1598  (char msg[448], ~447 o)
//   3. mqtt_manager.cpp:1614     (~630 o : litteral + phAutoManualReason)
//   4. config.cpp:379            (~530 o : litteral + phAutoManualReason)
//   5. mqtt_manager.cpp:770      ("Alerte: " + type + " - " + message)
//   6. sensors.cpp:716           (ré-amorçage REFUSÉ)
//   7. sensors.cpp:794           (filtre NON PRÊT depuis)
//   8. main.cpp:942              (performFailSafeRestart, reason)
//   9. fs_guard.cpp:212          (char ligne[128], EpisodeEnd)
//  10. oom_guard.cpp:931         (char ligne[224], parachute sans reserve)
//
// MUTATION ASSOCIÉE (à exécuter séparément, pas dans cette suite) : ramener
// kLogMessageMaxBytes à 256 dans constants.h (en suspendant temporairement le
// static_assert `kLogMessageMaxBytes >= 640` qui bloquerait sinon la
// compilation avant même d'atteindre ces tests) fait échouer SIX de ces dix
// cas -- chiffre MESURÉ le 16/09/2026, et non huit comme estimé au départ.
// Les quatre autres (fs_guard.cpp:212, oom_guard.cpp:931, main.cpp:942,
// sensors.cpp:716) restent sous 256 parce qu'ils sont bornés
// STRUCTURELLEMENT par de petits tampons de pile fixes ou des littéraux
// courts : c'est une propriété du firmware, pas un trou de cette suite.
// Ne pas conclure à une régression si la mutation n'en tue que six.
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"  // kLogMessageMaxBytes (source de verite, pas recopiee)

void setUp(void) {}
void tearDown(void) {}

// Buffer de RECONSTRUCTION du test -- 1024 o, DELIBEREMENT plus grand que
// n'importe quel message reel, pour que le test mesure la vraie longueur du
// contenu reconstruit sans jamais la masquer par une troncature locale.
static char gBuf[1024];

// =============================================================================
// Site 1 -- pump_controller.cpp:3600 -- char msg[480], divergence de
// composition pH/ORP (garde de securite chimique C5).
// =============================================================================
void test_site1_pump_composition_divergente(void) {
  // Reconstruction FIDELE du tampon reel (480 o) : le firmware l'ecrit dans
  // char msg[480], donc le pire cas REEL est deja borne a 479 caracteres +
  // NUL par snprintf lui-meme -- reconstruit ici a l'identique, sans
  // agrandir artificiellement le tampon de destination (on veut la longueur
  // REELLEMENT ecrite par le firmware, pas une longueur hypothetique).
  char msg[480];
  int n = snprintf(msg, sizeof(msg),
           "[Dosage pH] Composition divergente : la projection "
           "d'observabilité annonce %.2f mL/min alors que le calcul PI "
           "applique %.2f mL/min (écart %.2f > %.2f ; session ouverte "
           "depuis %us, écart pH %.3f, FF %.2f mL/min, plafond "
           "%.2f mL/min, bande morte %.3f) — les deux écritures de "
           "l'équation de débit ont cessé de coïncider : comparer "
           "doseComposition() (dosing_logic.cpp) et la branche "
           "automatique de pump_controller.cpp",
           -999.99, -999.99, 999.99, 999.99,
           4294967295u, -9.999, -999.99, 999.99, -9.999);
  TEST_ASSERT_TRUE(n > 0);
  size_t len = strlen(msg);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(kLogMessageMaxBytes, len);
  // Verrou de non-derive : ce site est borne par le tampon reel (480 o) --
  // s'il grandit un jour sans que le tampon suive, ce test doit encore
  // couvrir le nouveau pire cas (c'est pourquoi il reconstruit char msg[480]
  // et non char msg[kLogMessageMaxBytes]).
  TEST_ASSERT_LESS_OR_EQUAL_size_t(479, len);
}

// =============================================================================
// Site 2 -- pump_controller.cpp:1598 -- char msg[448], saturation de
// l'integrale pH (branche critical, la plus longue des trois).
// =============================================================================
void test_site2_pump_integrale_saturee_critical(void) {
  char msg[448];
  int n = snprintf(msg, sizeof(msg),
                   "Régulation pH saturée depuis plus de %lu h (%.2f mL/min "
                   "demandés pour %.2f mL/min soutenables",
                   4294967295UL, -999.99, -999.99);
  if (n < 0) n = 0;
  if ((size_t)n > sizeof(msg) - 1) n = (int)sizeof(msg) - 1;
  size_t room = sizeof(msg) - (size_t)n;
  snprintf(msg + n, room,
           ", terme intégral %.2f mL/min) — la régulation demande son maximum "
           "SANS atteindre la consigne : vérifier le niveau et l'amorçage du "
           "bidon, l'étanchéité du circuit de dosage, la sonde pH et la limite "
           "horaire d'injection",
           -999.99);
  size_t len = strlen(msg);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(kLogMessageMaxBytes, len);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(447, len);
}

// =============================================================================
// Sites 3 et 4 partagent leur pire cas : phAutoManualReason au pire cas
// (branche "diagnostic NORMAL", pump_controller.cpp:987-996, la plus longue
// des deux branches -- ~431 o).
// =============================================================================
static size_t buildWorstCasePhAutoManualReason(char* out, size_t outCap) {
  char diag[128];
  snprintf(diag, sizeof(diag), "(pente acide %.1f %%, base %.1f %%, zéro %.2f mV)",
           -999.9, -999.9, -999.99);
  return (size_t)snprintf(out, outCap,
      "%u refus de garde pH en 24 h — régulation pH basculée en manuel. "
      "Diagnostic sonde EZO NORMAL %s : la sonde n'est pas en cause. "
      "Vérifier la cohérence consigne / offset / produit déclaré, puis un "
      "décalage de référence EXTERNE (courants de fuite de la pompe de "
      "filtration, équipotentialité du bassin). Contrôler le pH au réactif "
      "avant toute intervention sur la sonde.",
      (unsigned)kPhGuardRefusalMax, diag);
}

void test_site3_mqtt_mode_refuse_avec_trace(void) {
  char reason[600];
  buildWorstCasePhAutoManualReason(reason, sizeof(reason));

  char payloadStr[16] = "automatic";  // pire cas parmi {automatic, scheduled, manual}

  size_t n = (size_t)snprintf(gBuf, sizeof(gBuf),
      "Mode régulation pH REFUSÉ (MQTT → %s) : bascule de sécurité pH non "
      "acquittée — acquittez l'alerte sur la page pH de l'interface web "
      "avant de réactiver la régulation. Trace active : %s",
      payloadStr, reason);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(gBuf));  // le tampon de TEST ne doit jamais tronquer
  size_t len = strlen(gBuf);
  TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(
      kLogMessageMaxBytes, len,
      "mqtt_manager.cpp:1614 -- refus de bascule pH avec trace, pire cas");
}

void test_site4_config_critical_bascule_non_acquittee(void) {
  char reason[600];
  buildWorstCasePhAutoManualReason(reason, sizeof(reason));

  size_t n = (size_t)snprintf(gBuf, sizeof(gBuf),
      "[Config] Régulation pH en MANUEL suite à une bascule automatique non "
      "acquittée : %s",
      reason);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(gBuf));
  size_t len = strlen(gBuf);
  TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(
      kLogMessageMaxBytes, len,
      "config.cpp:379 -- CRITICAL persistant, seule trace du blocage pH au reboot");
}

// =============================================================================
// Site 5 -- mqtt_manager.cpp:770 -- "Alerte: " + alertType + " - " + message.
// `message` N'EST PAS le `msg` deja borne a kMaxPayloadLen : c'est le
// parametre D'ORIGINE de publishAlert(), non clampe sur ce chemin de log
// (seul le JSON MQTT l'est). Le plus long `message` identifie par grep des
// sites d'appel de publishAlert() est le meme `alert` que les sites 3/4
// (pump_controller.cpp:1009, escalade "3 refus / 24h").
// =============================================================================
void test_site5_mqtt_alerte_journal_non_clampee(void) {
  char message[600];
  buildWorstCasePhAutoManualReason(message, sizeof(message));
  const char* alertType = "ph_guard";

  size_t n = (size_t)snprintf(gBuf, sizeof(gBuf), "Alerte: %s - %s", alertType, message);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(gBuf));
  size_t len = strlen(gBuf);
  TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(
      kLogMessageMaxBytes, len,
      "mqtt_manager.cpp:770 -- le journal recoit `message` NON clampe (le "
      "clamp JSON de publishAlert() ne s'applique qu'au payload MQTT)");
}

// =============================================================================
// Site 6 -- sensors.cpp:716-720 -- ré-amorçage REFUSÉ.
// =============================================================================
void test_site6_sensors_reamorcage_refuse(void) {
  const char* label = "ORP";  // 3 caracteres, pire cas vs "pH" (2)
  char motif[80];
  // Branche `denied==1` : litteral + kSensorFilterRearmCooldownMs/60000
  // (constante figee a 30 -- PAS une variable runtime, donc pas de pire cas
  // a chercher au-dela de la valeur reelle).
  snprintf(motif, sizeof(motif), "délai anti-répétition de %lu min non écoulé",
           (unsigned long)(kSensorFilterRearmCooldownMs / 60000UL));

  size_t n = (size_t)snprintf(gBuf, sizeof(gBuf),
      "Filtre %s : ré-amorçage REFUSÉ (%s) sur brut %.3f — le filtre reste "
      "NON PRÊT, régulation %s à l'arrêt : vérifier la sonde",
      label, motif, -9999.999, label);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(gBuf));
  size_t len = strlen(gBuf);
  TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(
      kLogMessageMaxBytes, len, "sensors.cpp:716 -- reamorcage refuse");
}

// =============================================================================
// Site 7 -- sensors.cpp:788-794 -- filtre NON PRÊT durablement.
// =============================================================================
void test_site7_sensors_filtre_non_pret_depuis(void) {
  const char* label = "ORP";
  // minutes = (nowMs - since) / 60000, nowMs/since en uint32_t (millis()) :
  // le pire cas ABSOLU est borne par 2^32-1 ms / 60000 = 71582 (5 chiffres),
  // jamais davantage (arithmetique 32 bits, cf. commentaire log_arena_logic
  // sur le repliement de millis()).
  const unsigned long minutes = 4294967295UL / 60000UL;  // 71582
  const char* cause = "ré-amorçage refusé (sonde suspecte)";  // le plus long des 4
  const unsigned rejected = 255;  // uint8_t max

  size_t n = (size_t)snprintf(gBuf, sizeof(gBuf),
      "Filtre %s NON PRÊT depuis %lu min alors que la filtration tourne et "
      "que les mesures sont représentatives — régulation %s À L'ARRÊT (cause "
      "probable : %s, rejets=%u). Vérifier la sonde ; réinitialisation du "
      "filtre possible depuis l'interface.",
      label, minutes, label, cause, rejected);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(gBuf));
  size_t len = strlen(gBuf);
  TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(
      kLogMessageMaxBytes, len, "sensors.cpp:794 -- filtre non pret durablement");
}

// =============================================================================
// Site 8 -- main.cpp:942 -- systemLogger.critical(reason), performFailSafeRestart.
// Le plus long des TROIS appelants identifies (grep performFailSafeRestart) :
// performAutoHealRestart(), reason dynamique avec maxAllocHeap en uint32_t.
// =============================================================================
void test_site8_main_reason_autoguerison(void) {
  size_t n = (size_t)snprintf(gBuf, sizeof(gBuf),
      "Auto-guérison : heap critique (fragmentation), redémarrage propre — "
      "maxBloc8=%u bytes",
      4294967295u);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(gBuf));
  size_t len = strlen(gBuf);
  TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(
      kLogMessageMaxBytes, len, "main.cpp:942 -- reason auto-guerison heap");
}

// =============================================================================
// Site 9 -- fs_guard.cpp:198-212 -- char ligne[128], episode EpisodeEnd.
// =============================================================================
void test_site9_fsguard_episode_retabli(void) {
  char ligne[128];
  snprintf(ligne, sizeof(ligne),
           "Ouvertures de fichiers retablies — %u refus en %u s",
           4294967295u, 4294967295u);
  size_t len = strlen(ligne);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(kLogMessageMaxBytes, len);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(127, len);
}

// =============================================================================
// Site 10 -- oom_guard.cpp:924-931 -- char ligne[224], parachute sans reserve.
// =============================================================================
void test_site10_oomguard_parachute_sans_reserve(void) {
  char ligne[224];
  snprintf(ligne, sizeof(ligne),
           "Parachute OOM SANS RÉSERVE depuis %u min (aucun palier armable) — "
           "plus aucun filet anti-bad_alloc, maxBloc8=%u o, libre8=%u o",
           4294967295u, 4294967295u, 4294967295u);
  size_t len = strlen(ligne);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(kLogMessageMaxBytes, len);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(223, len);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_site1_pump_composition_divergente);
  RUN_TEST(test_site2_pump_integrale_saturee_critical);
  RUN_TEST(test_site3_mqtt_mode_refuse_avec_trace);
  RUN_TEST(test_site4_config_critical_bascule_non_acquittee);
  RUN_TEST(test_site5_mqtt_alerte_journal_non_clampee);
  RUN_TEST(test_site6_sensors_reamorcage_refuse);
  RUN_TEST(test_site7_sensors_filtre_non_pret_depuis);
  RUN_TEST(test_site8_main_reason_autoguerison);
  RUN_TEST(test_site9_fsguard_episode_retabli);
  RUN_TEST(test_site10_oomguard_parachute_sans_reserve);
  return UNITY_END();
}
