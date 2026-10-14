// =============================================================================
// feature-164 — Consignes pH/ORP et horaires de filtration RÉGLABLES depuis
// les cartes : validation serveur + purge d'intégrale au changement RUNTIME
// =============================================================================
// POURQUOI CE FICHIER LIT DES SOURCES (patron DÉJÀ établi par ce dépôt :
// test_native_ws_config_parity, test_native_dose_mask)
// --------------------------------------------------------------------------
// `handleSaveConfig` (src/web_routes_config.cpp) et
// `PumpControllerClass::applyRegulationSpeedNow/updateDosingCore`
// (src/pump_controller.cpp) ne sont PAS compilables en natif (AsyncWebServer,
// ArduinoJson, FreeRTOS, configMutex) — `build_src_filter` de l'env `native`
// ne les inclut pas. La validation des bornes ORP/pH et la purge d'intégrale
// au changement de consigne vivent pourtant là, en glue non extraite.
//
// Ce fichier combine DEUX registres :
//   (A) exécution RÉELLE de la logique PURE déjà compilée en natif
//       (timeStringToMinutes / isMinutesInRange, schedule_logic.cpp ; les
//       constantes de constants.h) — un vrai test de comportement ;
//   (B) assertions STRUCTURELLES sur le texte des sources non compilables,
//       EXACTEMENT le patron des sentinelles de parité existantes : on
//       recherche la forme LITTÉRALE attendue (espaces retirés), jamais une
//       réimplémentation parallèle qui ne réagirait pas à une mutation du
//       fichier réel.
//
// CE QUE CETTE SUITE NE PROUVE PAS : la sémantique exacte d'ArduinoJson 7
// (`.as<float>()` sur une chaîne, `isfinite()` sur un NaN JSON) — non
// compilable en natif. Elle verrouille la PRÉSENCE de la forme sûre
// (`.as<float>()`, `isfinite(...)`) et l'ABSENCE de la forme dangereuse
// (affectation implicite), à vérifier sur cible par le plan de test manuel
// de la spec (scénario 2).
// =============================================================================

#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "schedule_logic.h"

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Lecture d'un fichier du dépôt — helper repris verbatim du patron de
// test_native_ws_config_parity / test_native_dose_mask : racine reconstruite
// depuis __FILE__, repli sur des préfixes relatifs.
// -----------------------------------------------------------------------------
static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_regulation_target_validation");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  const char* candidates[2] = {rooted[0] ? rooted : NULL, rel};
  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i >= 2) {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    } else {
      if (candidates[i] == NULL) continue;
      snprintf(path, sizeof(path), "%s", candidates[i]);
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

// Neutralise commentaires `//` et `/* */` (remplacés par des espaces, retours
// à la ligne conservés) AVANT toute recherche de motif — repris verbatim du
// patron de test_native_dose_mask. Indispensable ici : plusieurs commentaires
// du lot citent LITTÉRALEMENT le code dangereux qu'ils ont remplacé (ex. le
// commentaire de web_routes_config.cpp qui cite `mqttCfg.orpTarget =
// doc["orp_target"];` comme exemple du DÉFAUT corrigé) — sans ce nettoyage,
// une recherche de ce même motif dans le CODE RÉEL produirait un faux positif
// sur le commentaire qui le dénonce.
static void stripComments(char* s) {
  size_t i = 0;
  while (s[i] != '\0') {
    if (s[i] == '/' && s[i + 1] == '/') {
      while (s[i] != '\0' && s[i] != '\n') s[i++] = ' ';
    } else if (s[i] == '/' && s[i + 1] == '*') {
      s[i++] = ' ';
      s[i++] = ' ';
      while (s[i] != '\0' && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] != '\n') s[i] = ' ';
        i++;
      }
      if (s[i] != '\0') { s[i++] = ' '; s[i++] = ' '; }
    } else if (s[i] == '\'') {
      i++;
      if (s[i] == '\\' && s[i + 1] != '\0') i += 2;
      else if (s[i] != '\0') i++;
      if (s[i] == '\'') i++;
    } else if (s[i] == '"') {
      i++;
      while (s[i] != '\0' && s[i] != '"') {
        if (s[i] == '\\' && s[i + 1] != '\0') i++;
        i++;
      }
      if (s[i] != '\0') i++;
    } else {
      i++;
    }
  }
}

// Copie espaces/tabs/retours-ligne retirés — même garantie que les sentinelles
// de parité : une reformulation ne doit ni masquer un motif présent, ni en
// faire apparaître un absent.
static char* stripWhitespace(const char* src) {
  const size_t len = strlen(src);
  char* out = (char*)malloc(len + 1);
  if (out == NULL) return NULL;
  size_t n = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = src[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    out[n++] = c;
  }
  out[n] = '\0';
  return out;
}

static size_t countOccurrences(const char* haystack, const char* needle) {
  size_t count = 0;
  const char* p = haystack;
  const size_t nl = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) { count++; p += nl; }
  return count;
}

static const char* kHttpFile = "src/web_routes_config.cpp";
static const char* kPumpCppFile = "src/pump_controller.cpp";

static char* loadStrippedOrFail(const char* rel) {
  long len = 0;
  char* raw = readProjectFile(rel, &len);
  char msg[256];
  snprintf(msg, sizeof(msg),
           "FICHIER INTROUVABLE : %s. NE PAS DESACTIVER CE TEST.", rel);
  TEST_ASSERT_NOT_NULL_MESSAGE(raw, msg);
  if (raw == NULL) return NULL;
  stripComments(raw);
  char* stripped = stripWhitespace(raw);
  free(raw);
  TEST_ASSERT_NOT_NULL(stripped);
  return stripped;
}

// =============================================================================
// T1 — bornes de la consigne ORP [kOrpTargetPlausibleMin ; kOrpTargetPlausibleMax]
// =============================================================================

void test_t1_constantes_bornes_orp(void) {
  // Valeurs RÉELLES (compilées depuis constants.h, pas recopiées à la main
  // dans une doc qui pourrirait) : 300 et 950 mV.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 300.0f, kOrpTargetPlausibleMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 950.0f, kOrpTargetPlausibleMax);
  TEST_ASSERT_TRUE_MESSAGE(kOrpTargetPlausibleMin < kOrpTargetPlausibleMax,
      "borne basse >= borne haute : plage ORP vide ou inversée");
}

void test_t1_validation_orp_structurelle(void) {
  char* http = loadStrippedOrFail(kHttpFile);
  if (http == NULL) return;

  // Forme EXACTE attendue du refus (C2+C4) : isfinite() + bornes strictes
  // (`<`/`>`, PAS `<=`/`>=` — les bornes elles-mêmes sont des valeurs
  // ACCEPTÉES, le test T1 "limite exacte des deux côtés" en dépend).
  const char* condition =
      "if(!isfinite(requestedOrpTarget)||"
      "requestedOrpTarget<kOrpTargetPlausibleMin||"
      "requestedOrpTarget>kOrpTargetPlausibleMax){";
  const char* conditionPos = strstr(http, condition);
  TEST_ASSERT_NOT_NULL_MESSAGE(conditionPos,
      "T1 ROMPU : la condition de refus de la consigne ORP (isfinite + bornes "
      "STRICTES kOrpTargetPlausibleMin/Max) est absente ou reformulée dans "
      "src/web_routes_config.cpp. Une valeur hors plage, une chaîne, ou NaN "
      "serait acceptée EN SILENCE. NE PAS DESACTIVER CE TEST.");
  if (conditionPos == NULL) { free(http); return; }

  // Entre la condition et le PROCHAIN `return;`, AUCUNE mutation de
  // mqttCfg.orpTarget ni de doc["orp_target"] : le refus REFUSE, il n'écrête
  // jamais en silence (mutation possible : "retirer la borne ORP").
  const char* returnPos = strstr(conditionPos, "return;");
  TEST_ASSERT_NOT_NULL_MESSAGE(returnPos,
      "T1 : aucun `return;` trouvé après la condition de refus ORP — le refus "
      "ne refuse peut-être plus rien.");
  if (returnPos != NULL) {
    const size_t blockLen = (size_t)(returnPos - conditionPos);
    char* block = (char*)malloc(blockLen + 1);
    memcpy(block, conditionPos, blockLen);
    block[blockLen] = '\0';
    TEST_ASSERT_NULL_MESSAGE(strstr(block, "mqttCfg.orpTarget="),
        "T1 ROMPU : mqttCfg.orpTarget est écrit ENTRE la condition de refus "
        "et son `return;` — c'est un ÉCRÊTAGE SILENCIEUX, pas un refus.");
    TEST_ASSERT_NULL_MESSAGE(strstr(block, "doc[\"orp_target\"]="),
        "T1 ROMPU : doc[\"orp_target\"] est réécrit avant le `return;` du "
        "refus — un écrêtage silencieux déguisé.");
    free(block);
  }

  free(http);
}

void test_t1_orp_target_conversion_explicite_as_float(void) {
  char* http = loadStrippedOrFail(kHttpFile);
  if (http == NULL) return;

  // `.as<float>()` OBLIGATOIRE (sinon une chaîne JSON devient 0, défaut
  // d'origine documenté par le lot) : 3 sites — orpTargetWouldChange (C10),
  // la capture validée, et l'application finale à mqttCfg.orpTarget.
  const size_t n = countOccurrences(http, "doc[\"orp_target\"].as<float>()");
  char msg[200];
  snprintf(msg, sizeof(msg),
           "T1 : doc[\"orp_target\"].as<float>() apparaît %u fois (attendu "
           ">= 2). Sans .as<float>(), une chaîne JSON devient 0 à l'insu de "
           "l'utilisateur.", (unsigned)n);
  TEST_ASSERT_TRUE_MESSAGE(n >= 2, msg);

  // La forme DANGEREUSE (affectation implicite, sans .as<float>()) ne doit
  // JAMAIS réapparaître sur le chemin d'application.
  TEST_ASSERT_NULL_MESSAGE(strstr(http, "mqttCfg.orpTarget=doc[\"orp_target\"];"),
      "T1 ROMPU : mqttCfg.orpTarget est à nouveau affecté SANS .as<float>() — "
      "régression du défaut d'origine (une chaîne JSON vaudrait 0).");

  free(http);
}

// =============================================================================
// T2 — bornes de la consigne pH (NON-RÉGRESSION, v2.25.0, inchangé par ce lot)
// =============================================================================

void test_t2_constantes_bornes_ph(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.20f, kPhTargetMin);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.50f, kPhTargetMax);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, kPhTargetStep);
}

void test_t2_validation_et_alignement_ph_structurels(void) {
  char* http = loadStrippedOrFail(kHttpFile);
  if (http == NULL) return;

  const char* condition =
      "if(!isfinite(requestedPhTarget)||"
      "requestedPhTarget<kPhTargetMin||"
      "requestedPhTarget>kPhTargetMax){";
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(http, condition),
      "T2 RÉGRESSION : la condition de refus de la consigne pH (v2.25.0) est "
      "absente ou reformulée. NE PAS DESACTIVER CE TEST.");

  const char* snap =
      "snappedPhTarget=roundf(requestedPhTarget/kPhTargetStep)*kPhTargetStep;";
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(http, snap),
      "T2 RÉGRESSION : l'alignement de la consigne pH sur kPhTargetStep "
      "(grille de 0,10) a disparu ou changé de forme.");

  free(http);
}

// =============================================================================
// T3 — horaires de filtration : enjambement de minuit ACCEPTÉ,
// début == fin REFUSÉ
// =============================================================================

// (A) comportement RÉEL de la logique pure déjà compilée en natif : une
// plage "22:00"-"06:00" (fin < début) ENJAMBE MINUIT, volontaire.
void test_t3_enjambement_minuit_accepte_logique_reelle(void) {
  const int startMin = timeStringToMinutes("22:00");
  const int endMin = timeStringToMinutes("06:00");
  TEST_ASSERT_EQUAL_INT(1320, startMin);
  TEST_ASSERT_EQUAL_INT(360, endMin);
  // equalMeansAlways=false : patron filtration (schedule_logic.h).
  TEST_ASSERT_TRUE_MESSAGE(isMinutesInRange(1350, startMin, endMin, false),
      "22:30 doit être DANS une plage 22:00-06:00 (soir)");
  TEST_ASSERT_TRUE_MESSAGE(isMinutesInRange(100, startMin, endMin, false),
      "01:40 doit être DANS une plage 22:00-06:00 (matin)");
  TEST_ASSERT_FALSE_MESSAGE(isMinutesInRange(720, startMin, endMin, false),
      "12:00 doit être HORS d'une plage 22:00-06:00");
}

// (B) comportement RÉEL : début == fin ne tourne JAMAIS pour la filtration
// (equalMeansAlways=false) — c'est EXACTEMENT le défaut muet que C11 corrige
// en le transformant en REFUS explicite côté serveur.
void test_t3_debut_egal_fin_ne_tourne_jamais_logique_reelle(void) {
  const int m = timeStringToMinutes("08:00");
  TEST_ASSERT_EQUAL_INT(480, m);
  TEST_ASSERT_FALSE_MESSAGE(isMinutesInRange(480, m, m, false),
      "start==end doit toujours renvoyer false pour la filtration "
      "(equalMeansAlways=false) — silencieux sans le refus C11");
}

// (C) assertions STRUCTURELLES sur le refus C11 ajouté par ce lot, et sur
// l'ABSENCE d'un refus illégitime de l'enjambement de minuit.
void test_t3_refus_structurel_debut_egal_fin_sans_casser_minuit(void) {
  char* http = loadStrippedOrFail(kHttpFile);
  if (http == NULL) return;

  const char* checkPos = strstr(http, "if(startMin==endMin){");
  TEST_ASSERT_NOT_NULL_MESSAGE(checkPos,
      "T3/C11 ROMPU : le refus explicite `startMin==endMin` a disparu — la "
      "filtration pourrait à nouveau ne jamais tourner SANS message. NE PAS "
      "DESACTIVER CE TEST.");
  if (checkPos == NULL) { free(http); return; }

  const char* returnPos = strstr(checkPos, "return;");
  TEST_ASSERT_NOT_NULL(returnPos);
  if (returnPos != NULL) {
    const size_t blockLen = (size_t)(returnPos - checkPos);
    char* block = (char*)malloc(blockLen + 1);
    memcpy(block, checkPos, blockLen);
    block[blockLen] = '\0';
    TEST_ASSERT_NULL_MESSAGE(strstr(block, "filtrationCfg.start="),
        "T3 ROMPU : filtrationCfg.start est écrit avant le `return;` du refus "
        "start==end — écrêtage silencieux au lieu d'un refus.");
    TEST_ASSERT_NULL_MESSAGE(strstr(block, "filtrationCfg.end="),
        "T3 ROMPU : filtrationCfg.end est écrit avant le `return;` du refus "
        "start==end — écrêtage silencieux au lieu d'un refus.");
    free(block);
  }

  // ⛔ Chercher dans TOUT le bloc de validation (entre le test de format et
  // l'application) qu'AUCUN refus illégitime de `fin < début` n'a été ajouté.
  // Ce cas enjambe minuit — volontaire, cf. schedule_logic.cpp.
  const char* blockStart = strstr(http, "if(startMin<0||endMin<0){");
  const char* blockEnd = strstr(http, "if(!doc[\"filtration_start\"].isNull()){filtrationCfg.start=");
  TEST_ASSERT_NOT_NULL(blockStart);
  TEST_ASSERT_NOT_NULL(blockEnd);
  if (blockStart != NULL && blockEnd != NULL && blockEnd > blockStart) {
    const size_t len = (size_t)(blockEnd - blockStart);
    char* region = (char*)malloc(len + 1);
    memcpy(region, blockStart, len);
    region[len] = '\0';
    TEST_ASSERT_NULL_MESSAGE(strstr(region, "startMin>endMin"),
        "T3 ROMPU : un refus `startMin>endMin` a été ajouté — cela CASSERAIT "
        "l'enjambement de minuit, cas d'usage réel (ex: filtration de nuit). "
        "⛔ fin < début DOIT rester accepté.");
    TEST_ASSERT_NULL_MESSAGE(strstr(region, "endMin<startMin"),
        "T3 ROMPU : un refus `endMin<startMin` a été ajouté — cela CASSERAIT "
        "l'enjambement de minuit. ⛔ fin < début DOIT rester accepté.");
    free(region);
  }

  free(http);
}

// =============================================================================
// T4 — C5+C6 : purge de l'intégrale pH au changement RUNTIME de consigne,
// SANS toucher _sessionOnMs ni les compteurs (« on efface une demande, jamais
// une dette »)
// =============================================================================

// C6 — le drapeau consommé en tête d'update() force la purge (forcePurge=true),
// même sans changement de vitesse de régulation.
void test_t4_c6_consommation_force_purge(void) {
  char* pump = loadStrippedOrFail(kPumpCppFile);
  if (pump == NULL) return;

  const char* expected =
      "if(_phTargetChangeRequested.exchange(false)){"
      "applyRegulationSpeedNow(true);"
      "}";
  char msg[400];
  snprintf(msg, sizeof(msg),
           "T4/C6 ROMPU : le bloc de consommation de _phTargetChangeRequested "
           "n'est plus EXACTEMENT `%s` dans src/pump_controller.cpp (il a soit "
           "perdu forcePurge=true — l'intégrale ne serait plus jamais purgée au "
           "changement de consigne seul —, soit gagné un effet de bord "
           "supplémentaire — cf. test C5 ci-dessous). NE PAS DESACTIVER CE "
           "TEST.", expected);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(pump, expected), msg);

  // La purge elle-même doit être CONDITIONNÉE par `forcePurge`, pas seulement
  // par `phTuningChanged` (sinon un changement de CONSIGNE seul, sans
  // changement de vitesse, ne purgerait jamais rien).
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(pump, "constboolshouldPurge=phTuningChanged||forcePurge;"),
      "T4/C6 ROMPU : `shouldPurge` n'est plus `phTuningChanged || forcePurge` "
      "dans applyRegulationSpeedNow — forcePurge serait ignoré.");

  free(pump);
}

// C5 — le bloc de consommation ne touche NI _sessionOnMs NI les compteurs de
// dosage : on efface une DEMANDE (l'intégrale), jamais une DETTE (ce qui a été
// physiquement injecté).
void test_t4_c5_ne_remet_pas_a_zero_la_comptabilite(void) {
  char* pump = loadStrippedOrFail(kPumpCppFile);
  if (pump == NULL) return;

  // Le bloc de consommation (voir test C6 ci-dessus) est EXACTEMENT ces trois
  // instructions — strstr de la forme complète prouve qu'aucune instruction
  // supplémentaire (reset de compteur) n'a été glissée dedans : une égalité
  // stricte de bloc, pas seulement une recherche de sous-chaîne dans tout le
  // fichier (qui laisserait passer un ajout AILLEURS mais détecté par cette
  // forme figée).
  const char* exact =
      "if(_phTargetChangeRequested.exchange(false)){"
      "applyRegulationSpeedNow(true);"
      "}";
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(pump, exact),
      "T4/C5 : bloc de consommation introuvable (doublon du test C6 — si ce "
      "test échoue seul, le bloc contient un effet de bord en plus de l'appel "
      "attendu : _sessionOnMs ou un compteur a été ajouté. Motif du gate : "
      "« on efface une demande, jamais une dette ».");

  free(pump);
}

// C5+C6 — le DÉCLENCHEUR, côté web_routes_config.cpp : armé sur un changement
// RÉEL de valeur (comparaison phTargetBefore/mqttCfg.phTarget APRÈS
// application), JAMAIS sur la seule PRÉSENCE de la clé ph_target (l'interface
// la renvoie à CHAQUE enregistrement).
void test_t4_declencheur_sur_changement_reel_pas_presence_de_cle(void) {
  char* http = loadStrippedOrFail(kHttpFile);
  if (http == NULL) return;

  const char* expected =
      "if(fabsf(mqttCfg.phTarget-phTargetBefore)>0.001f){"
      "PumpController.requestPhTargetChange();"
      "}";
  char msg[400];
  snprintf(msg, sizeof(msg),
           "T4 ROMPU : le déclencheur de purge n'est plus EXACTEMENT `%s` "
           "dans src/web_routes_config.cpp. Si remplacé par une condition sur "
           "`!doc[\"ph_target\"].isNull()` (présence de clé), CHAQUE "
           "enregistrement purgerait l'intégrale — y compris quand l'interface "
           "renvoie la MÊME consigne. NE PAS DESACTIVER CE TEST.", expected);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(http, expected), msg);

  // Garde-fou direct : le déclencheur ne doit JAMAIS être conditionné par
  // `doc["ph_target"].isNull()` immédiatement avant l'appel — recherche
  // ciblée du motif dangereux à proximité de l'appel.
  const char* callPos = strstr(http, "PumpController.requestPhTargetChange();");
  TEST_ASSERT_NOT_NULL(callPos);
  if (callPos != NULL) {
    const size_t back = (size_t)(callPos - http);
    const size_t windowStart = back > 200 ? back - 200 : 0;
    char window[256];
    const size_t windowLen = back - windowStart;
    size_t copyLen = windowLen < sizeof(window) - 1 ? windowLen : sizeof(window) - 1;
    memcpy(window, http + windowStart, copyLen);
    window[copyLen] = '\0';
    TEST_ASSERT_NULL_MESSAGE(strstr(window, "!doc[\"ph_target\"].isNull())"),
        "T4 ROMPU : requestPhTargetChange() semble armé par la PRÉSENCE de la "
        "clé ph_target (isNull()) plutôt que par un changement RÉEL de "
        "valeur.");
  }

  free(http);
}

// requestPhTargetChange() : fonction à EFFET UNIQUE (poser le drapeau
// atomique). Aucune écriture de _phIntegral/_sessionOnMs depuis ce contexte
// async (invariant v2.26.0 : seule loopTask écrit phPID/_phIntegral).
void test_t4_requestphtargetchange_effet_unique(void) {
  char* pump = loadStrippedOrFail(kPumpCppFile);
  if (pump == NULL) return;

  const char* exact =
      "voidPumpControllerClass::requestPhTargetChange(){"
      "_phTargetChangeRequested.store(true);"
      "}";
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(pump, exact),
      "T4 ROMPU : requestPhTargetChange() ne se limite plus à poser le "
      "drapeau atomique _phTargetChangeRequested — un effet de bord a été "
      "ajouté dans un contexte ASYNC (web), en violation de l'invariant "
      "v2.26.0 (seule loopTask écrit phPID/_phIntegral).");

  free(pump);
}

// =============================================================================
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_t1_constantes_bornes_orp);
  RUN_TEST(test_t1_validation_orp_structurelle);
  RUN_TEST(test_t1_orp_target_conversion_explicite_as_float);
  RUN_TEST(test_t2_constantes_bornes_ph);
  RUN_TEST(test_t2_validation_et_alignement_ph_structurels);
  RUN_TEST(test_t3_enjambement_minuit_accepte_logique_reelle);
  RUN_TEST(test_t3_debut_egal_fin_ne_tourne_jamais_logique_reelle);
  RUN_TEST(test_t3_refus_structurel_debut_egal_fin_sans_casser_minuit);
  RUN_TEST(test_t4_c6_consommation_force_purge);
  RUN_TEST(test_t4_c5_ne_remet_pas_a_zero_la_comptabilite);
  RUN_TEST(test_t4_declencheur_sur_changement_reel_pas_presence_de_cle);
  RUN_TEST(test_t4_requestphtargetchange_effet_unique);
  return UNITY_END();
}
