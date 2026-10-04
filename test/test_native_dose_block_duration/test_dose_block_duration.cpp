// =============================================================================
// Tests natifs — feature-138 : durée du blocage de dosage (dose_block_duration_logic.h)
// =============================================================================
// CE QUE CE LOT PROTÈGE
// --------------------
// `doseBlockElapsedS(nowMs, sinceMs, blocked, capS)` est le module PUR (ADR-0017)
// qui transforme deux horodatages millis() en une durée bornée, en secondes.
// Il doit rester exact à travers le repliement de millis() (24,8 j) et ne
// JAMAIS confondre « sinceMs == 0 » avec « pas de refus » : la milliseconde 0
// est une valeur LÉGITIME après le boot et après chaque repliement — c'est
// très exactement le défaut documenté de `_sensorFaultSinceMs`
// (pump_controller.h), que ce module ne doit pas reproduire.
//
// LA REDATATION ELLE-MÊME (remise à zéro de `_refusalCodeSinceMs` à chaque
// changement de `_lastRefusalCode`, y compris vers kDoseBlockedCodeNone) vit
// dans `PumpControllerClass::setRefusalCode()` (src/pump_controller.cpp), qui
// N'EST PAS dans `build_src_filter` de l'env `native` (il tire millis(),
// FreeRTOS, les autres managers). Pour rendre ce contrat testable nativement,
// cette suite reproduit l'algorithme EXACT de `setRefusalCode()` dans un petit
// harnais local (`RefusalTracker` / `trackerSetCode`) — même garde, même ordre
// d'écriture (date AVANT code). Le harnais n'est PAS une preuve que la
// production fait la même chose : c'est pourquoi `test_z_*` ci-dessous relit le
// TEXTE de `src/pump_controller.cpp` pour vérifier que le harnais n'a pas
// divergé du contrat réel (même garde, même ordre, un seul site d'écriture).
//
// CE QUE CETTE SUITE NE PROUVE PAS : que `getPhDoseBlockedForS()` appelle bien
// `doseBlockElapsedS` avec les bons arguments sur cible — vérifié par lecture
// (fait, cf. rapport) et par le plan de test manuel de la spec.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dose_block_duration_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Repris littéralement de constants.h / pump_controller.h (ces en-têtes ne
// sont pas inclus directement : ils tirent LittleFS/FreeRTOS/Arduino).
static const uint8_t  kDoseBlockedCodeNone  = 0;
static const uint32_t kDoseBlockedForMaxS   = 86400;  // 24h

// =============================================================================
// Harnais — reproduit l'algorithme de PumpControllerClass::setRefusalCode()
// =============================================================================
struct RefusalTracker {
  uint8_t  code    = kDoseBlockedCodeNone;
  uint32_t sinceMs = 0;
};

// Même garde, même ORDRE d'écriture (date AVANT code) que la production.
static void trackerSetCode(RefusalTracker& t, uint8_t code, uint32_t nowMs) {
  if (code != t.code) {
    t.sinceMs = nowMs;
  }
  t.code = code;
}

static uint32_t trackerElapsedS(const RefusalTracker& t, uint32_t nowMs) {
  return doseBlockElapsedS(nowMs, t.sinceMs, t.code != kDoseBlockedCodeNone,
                           kDoseBlockedForMaxS);
}

// =============================================================================
// D1 — jamais bloqué depuis le boot : pas de durée inventée
// =============================================================================
void test_jamais_bloque_depuis_le_boot_pas_de_duree_inventee(void) {
  RefusalTracker t;  // état initial : code = None, sinceMs = 0
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, 0));
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, 123456));
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, 4294967295u));
}

// =============================================================================
// ⚠️ La validité n'est PAS portée par sinceMs != 0 — milliseconde 0 légitime.
// =============================================================================
void test_validite_non_portee_par_sinceMs_non_nul(void) {
  // Code de refus posé à la toute première milliseconde du boot (sinceMs = 0,
  // PAS une sentinelle « pas d'épisode »). 5000 ms plus tard : 5 s écoulées.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      5, doseBlockElapsedS(5000u, 0u, true, kDoseBlockedForMaxS),
      "sinceMs == 0 doit etre traite comme un horodatage LEGITIME quand "
      "blocked == true, jamais comme « pas de refus » (c'est code, pas "
      "sinceMs, qui porte cette decision).");

  // Et juste après le repliement : sinceMs == 0 de nouveau (nouvelle ère),
  // nowMs == 0 aussi (blocage qui démarre pile au repliement) → 0 s, pas une
  // erreur, pas une durée négative déguisée.
  TEST_ASSERT_EQUAL_UINT32(0, doseBlockElapsedS(0u, 0u, true, kDoseBlockedForMaxS));
}

// =============================================================================
// D1 — changement de code de refus → la durée repart
// =============================================================================
void test_changement_de_code_redate(void) {
  RefusalTracker t;
  trackerSetCode(t, 5 /* un code de refus quelconque */, 1000u);
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, 1000u));
  TEST_ASSERT_EQUAL_UINT32(49, trackerElapsedS(t, 50000u));  // 49 s plus tard

  // Changement VERS UN AUTRE code de refus, à nowMs = 50000 : la durée repart.
  trackerSetCode(t, 7, 50000u);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, trackerElapsedS(t, 50000u),
      "Un changement de code de refus doit remettre la duree a zero "
      "IMMEDIATEMENT, pas la laisser heriter de l'ancienne date.");
  TEST_ASSERT_EQUAL_UINT32(3, trackerElapsedS(t, 53000u));
}

// =============================================================================
// D1 — retour à « plus de refus » → la durée repart AUSSI
// =============================================================================
void test_retour_a_non_bloque_redate_aussi(void) {
  RefusalTracker t;
  trackerSetCode(t, 5, 0u);
  TEST_ASSERT_EQUAL_UINT32(10, trackerElapsedS(t, 10000u));

  // Retour à kDoseBlockedCodeNone : plus aucun refus. blocked == false donc
  // l'API renvoie 0 — mais la remise à zéro de sinceMs doit AUSSI avoir eu
  // lieu (vérifié ci-dessous par la ré-apparition d'un refus juste après).
  trackerSetCode(t, kDoseBlockedCodeNone, 10000u);
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, 10000u));
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, 99999u));

  // Un NOUVEAU refus apparaît 500 ms après le retour à « non bloqué ». S'il
  // avait hérité de l'ancienne date (0), la durée afficherait 10,5 s au lieu
  // de 0 : exactement le mensonge que la spec interdit ("y compris vers
  // l'absence de refus").
  trackerSetCode(t, 5, 10500u);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, trackerElapsedS(t, 10500u),
      "La remise a zero sur 'retour a non bloque' doit avoir pose une "
      "nouvelle date : un refus qui reapparait ensuite ne doit PAS heriter "
      "d'une date anterieure au passage par 'non bloque'.");
  TEST_ASSERT_EQUAL_UINT32(2, trackerElapsedS(t, 12500u));
}

// =============================================================================
// D1 — oscillation du code → la durée repart, sans lissage ni moyenne
// =============================================================================
void test_oscillation_du_code_sans_lissage(void) {
  RefusalTracker t;
  uint32_t now = 0;

  trackerSetCode(t, 5, now);                 // now=0      code=5
  now += 200; trackerSetCode(t, 7, now);      // now=200    code=7
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, now));
  now += 300; trackerSetCode(t, kDoseBlockedCodeNone, now);  // now=500 none
  TEST_ASSERT_EQUAL_UINT32(0, trackerElapsedS(t, now));
  now += 100; trackerSetCode(t, 5, now);      // now=600    code=5 de nouveau
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, trackerElapsedS(t, now),
      "Chaque bascule doit repartir de zero : aucune moyenne ni lissage sur "
      "la fenetre d'oscillation (500 ms de va-et-vient ne doivent pas se "
      "retrouver dans la duree affichee apres la derniere bascule).");

  // 900 ms après la dernière bascule (now=600), sans nouveau changement :
  // (900 ms écoulés) / 1000 -> 0 s pile, PAS 1 s. Un calcul qui ignorerait la
  // redatation (sinceMs resté à 0 depuis le tout premier changement, à
  // now=0) daterait l'écart depuis l'origine : 600+900 = 1500 ms -> 1 s, un
  // résultat DIFFÉRENT du bon (0 s) qui révèle la perte de redatation.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, trackerElapsedS(t, now + 900),
      "La duree apres la derniere bascule doit se calculer depuis la "
      "DERNIERE redatation (now=600), pas depuis une date anterieure a "
      "l'oscillation (sinceMs reste a 0 donnerait 1 s ici, pas 0).");
}

// =============================================================================
// D1 — repliement de millis() pendant un blocage long (franchir 2^32)
// =============================================================================
void test_repliement_millis_pendant_blocage_long(void) {
  // Blocage posé juste avant le repliement, constaté juste après.
  const uint32_t sinceMs = 4294960000u;        // ~7,296 s avant UINT32_MAX+1
  const uint32_t nowMs   = 5000u;              // après repliement (nouvelle ère)
  // Ecart réel : (2^32 - 4294960000) + 5000 = 7296 + 5000 = 12296 ms → 12 s.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      12, doseBlockElapsedS(nowMs, sinceMs, true, kDoseBlockedForMaxS),
      "L'arithmetique non signee doit rester exacte A TRAVERS le repliement "
      "de millis() (24,8 j) : (uint32_t)(nowMs - sinceMs) doit retomber sur "
      "le bon ecart meme quand nowMs < sinceMs en lecture naive.");

  // Cas limite : repliement EXACT (sinceMs juste avant 0, nowMs = 0 pile).
  TEST_ASSERT_EQUAL_UINT32(0, doseBlockElapsedS(0u, 4294967295u, true,
                                                kDoseBlockedForMaxS));

  // Blocage qui survit plusieurs secondes après un repliement proche de zéro.
  TEST_ASSERT_EQUAL_UINT32(3, doseBlockElapsedS(3000u, 4294967295u, true,
                                                kDoseBlockedForMaxS));
}

// Harnais + repliement combinés : le tracker lui-même traverse un repliement
// sans qu'un changement de code intervienne (le cas réel d'un blocage qui dure
// plus de 24,8 j sans jamais changer de cause).
void test_repliement_millis_avec_tracker_sans_changement_de_code(void) {
  RefusalTracker t;
  trackerSetCode(t, 5, 4294960000u);   // refus posé juste avant le repliement
  // 12,296 s plus tard (après repliement), toujours le même code : 12 s.
  TEST_ASSERT_EQUAL_UINT32(12, trackerElapsedS(t, 5000u));
}

// =============================================================================
// D1 — saturation au clamp kDoseBlockededForMaxS (86400 s)
// =============================================================================
void test_saturation_au_clamp(void) {
  // 100 000 s écoulées (> 24h) : clampées à 86400.
  TEST_ASSERT_EQUAL_UINT32(kDoseBlockedForMaxS,
                           doseBlockElapsedS(100000000u, 0u, true,
                                             kDoseBlockedForMaxS));
  // Pile à la borne : pas clampé en-dessous, ni off-by-one au-dessus.
  TEST_ASSERT_EQUAL_UINT32(kDoseBlockedForMaxS,
                           doseBlockElapsedS(kDoseBlockedForMaxS * 1000u, 0u,
                                             true, kDoseBlockedForMaxS));
  TEST_ASSERT_EQUAL_UINT32(kDoseBlockedForMaxS - 1,
                           doseBlockElapsedS((kDoseBlockedForMaxS - 1) * 1000u,
                                             0u, true, kDoseBlockedForMaxS));
}

// =============================================================================
// z — Non-régression textuelle : le harnais ci-dessus ne doit pas avoir
// divergé du contrat RÉEL de `PumpControllerClass::setRefusalCode()`. Ce
// n'est pas une preuve comportementale (pump_controller.cpp n'est pas compilé
// en natif) mais un filet : si la garde ou l'ordre d'écriture change dans la
// production sans que ce test soit mis à jour, l'écart devient visible.
// =============================================================================
static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  const char* f = __FILE__;
  const char* m = strstr(f, "test/test_native_dose_block_duration");
  if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
    const size_t n = (size_t)(m - f);
    memcpy(rooted, f, n);
    rooted[n] = '\0';
    strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
  }
  const char* candidates[2] = {rooted[0] ? rooted : NULL, rel};
  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i < 2) {
      if (candidates[i] == NULL) continue;
      snprintf(path, sizeof(path), "%s", candidates[i]);
    } else {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) continue;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 2000000L) { fclose(fp); continue; }
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

void test_z_le_harnais_ne_diverge_pas_du_contrat_source(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/pump_controller.cpp introuvable : le harnais de ce test "
           "reproduit setRefusalCode() et ne peut plus etre confronte au "
           "contrat reel. NE PAS DESACTIVER CE TEST.");
  if (src == NULL) return;

  const char* fn = strstr(src, "void PumpControllerClass::setRefusalCode(");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      fn, "setRefusalCode() introuvable dans pump_controller.cpp (renommee ?) "
          "— le harnais de ce test n'a plus de contrat a refleter.");
  if (fn == NULL) { free(src); return; }

  // Même garde : le changement de code est testé avant l'écriture de la date.
  const char* guard = strstr(fn, "if (code != _lastRefusalCode[pumpIndex])");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      guard, "La garde 'if (code != _lastRefusalCode[pumpIndex])' a disparu "
             "de setRefusalCode(). QUE FAIRE : soit la restaurer en "
             "production, soit mettre a jour trackerSetCode() ET tous les "
             "tests de ce fichier qui en dependent (redatation sur "
             "changement de code).");

  // Ordre : _refusalCodeSinceMs posé AVANT _lastRefusalCode (invariant de la
  // spec feature-138 — un lecteur async ne doit jamais voir un code neuf
  // associe a une date ancienne).
  const char* dateWrite = strstr(fn, "_refusalCodeSinceMs[pumpIndex] =");
  const char* codeWrite = strstr(fn, "_lastRefusalCode[pumpIndex] = code;");
  TEST_ASSERT_NOT_NULL_MESSAGE(dateWrite, "Ecriture de _refusalCodeSinceMs introuvable.");
  TEST_ASSERT_NOT_NULL_MESSAGE(codeWrite, "Ecriture de _lastRefusalCode introuvable.");
  if (dateWrite != NULL && codeWrite != NULL) {
    TEST_ASSERT_TRUE_MESSAGE(
        dateWrite < codeWrite,
        "INVARIANT D'ORDRE ROMPU : _lastRefusalCode doit etre ecrit APRES "
        "_refusalCodeSinceMs, sans quoi un lecteur async entre les deux "
        "assignations verrait un code NEUF avec une date ANCIENNE — le "
        "mensonge ('bloque depuis 9h' pour un blocage d'une seconde) que "
        "feature-138 combat explicitement.");
  }

  // Site d'écriture UNIQUE : un seul `_lastRefusalCode[...] = ...;` dans tout
  // le fichier (hors déclaration/initialisation en .h, qui n'est pas ici).
  size_t writeCount = 0;
  for (const char* p = strstr(src, "_lastRefusalCode[");
       p != NULL; p = strstr(p + 1, "_lastRefusalCode[")) {
    const char* eq = strchr(p, '=');
    const char* nl = strchr(p, '\n');
    if (eq != NULL && nl != NULL && eq < nl && eq[1] != '=') {
      // Exclut les comparaisons `!=` et les simples lectures.
      if (!(eq > p && eq[-1] == '!')) writeCount++;
    }
  }
  char msg[400];
  snprintf(msg, sizeof(msg),
           "%u site(s) d'ecriture de _lastRefusalCode trouve(s) dans "
           "pump_controller.cpp, un SEUL attendu (setRefusalCode). Un "
           "nouveau site d'ecriture directe contournerait la redatation.",
           (unsigned)writeCount);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(1, writeCount, msg);

  free(src);
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_jamais_bloque_depuis_le_boot_pas_de_duree_inventee);
  RUN_TEST(test_validite_non_portee_par_sinceMs_non_nul);
  RUN_TEST(test_changement_de_code_redate);
  RUN_TEST(test_retour_a_non_bloque_redate_aussi);
  RUN_TEST(test_oscillation_du_code_sans_lissage);
  RUN_TEST(test_repliement_millis_pendant_blocage_long);
  RUN_TEST(test_repliement_millis_avec_tracker_sans_changement_de_code);
  RUN_TEST(test_saturation_au_clamp);
  RUN_TEST(test_z_le_harnais_ne_diverge_pas_du_contrat_source);
  return UNITY_END();
}
