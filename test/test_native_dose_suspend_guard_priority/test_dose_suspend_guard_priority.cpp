// =============================================================================
// GARDE STRUCTURELLE — bug-172, correctif de revue (gate pool-chemistry,
// « RANG 0 DE LA CASCADE ») : une cause de GARDE CHIMIQUE n'est JAMAIS
// écrasée par la suspension globale (eau / étalonnage / stabilisation /
// assistant de débit).
// =============================================================================
// CE QUE CE LOT PROTÈGE — la PROPRIÉTÉ en trois phases exigée par le gate :
//
//   PHASE 1 (pose)      : une garde chimique (ex. pH sous le plancher) prend
//                         la propriété de la cause affichée via
//                         setGuardRefusalCause() → _refusalCauseFromGuard
//                         passe à true, code = sentinelle 250.
//   PHASE 2 (suspension) : la filtration s'arrête (ou toute autre des 4
//                         conditions de suspension globale). updateDosingCore()
//                         appelle postSuspendCause() À CHAQUE TICK, pour les
//                         DEUX pompes, PENDANT TOUTE LA DURÉE de la coupure —
//                         potentiellement des heures. tickPhGuardSurveillance()
//                         NE TOURNE PAS pendant cette phase (elle est placée
//                         APRÈS le `return` qui ferme ce bloc) : ses latches
//                         (dont _refusalCauseFromGuard) sortent donc
//                         STRICTEMENT INCHANGÉS d'un tick à l'autre.
//   PHASE 3 (reprise)    : la filtration repart. isPhGuardBlocking(true) est
//                         de nouveau évalué AVANT canDose() : tant qu'il
//                         bloque, canDose() n'est PAS appelé — rien ne peut
//                         reposer la cause autrement que par un FRONT de
//                         tickPhGuardSurveillance(), qui ne se représente pas
//                         si le pH est resté sous le seuil pendant toute la
//                         coupure.
//
// ⇒ La SEULE garantie qui fait tenir les trois phases ensemble est que
// postSuspendCause() ne touche JAMAIS l'état (_lastRefusalCode,
// _lastRefusalCause, _refusalCauseFromGuard) tant que
// _refusalCauseFromGuard[pumpIndex] est vrai — à CHAQUE appel, donc pour
// N'IMPORTE QUEL NOMBRE de ticks de la phase 2 (la fonction ne boucle pas :
// un appel unitaire prouvé sûr vaut pour tous les appels répétés, par
// induction sur un corps de fonction strictement linéaire, sans early state
// mutable entre deux appels).
//
// POURQUOI UN TEST STRUCTUREL (ET PAS UN TEST DE COMPORTEMENT)
// --------------------------------------------------------------
// `PumpControllerClass` n'est PAS instanciable en natif : elle tire
// FreeRTOS/LEDC/WiFi/l'I2C des EZO/LittleFS/ArduinoJson (via pump_controller.h
// → config.h, mqtt_manager.h, filtration.h, sensors.h…) et aucun de ces
// sous-systèmes n'a de shim natif pour ce fichier — `pump_controller.cpp`
// N'EST PAS dans build_src_filter de l'env `native` (platformio.ini) et ne
// peut structurellement pas l'être sans un chantier de mocking hors du
// périmètre de ce lot. C'est très exactement la situation documentée en tête
// de `test_native_ph_guard_causes` / `test_native_dose_banner_silence` /
// `test_native_ota_order_guard` : patron IMPOSÉ, lecture du TEXTE réellement
// livré (jamais une copie), `readProjectFile()` + analyse structurelle.
//
// Aucune mutation sur `evaluateSuspendCause()` (src/dosing_logic.cpp, déjà
// testée en natif par `test_native_dosing`) n'aurait pu attraper ce défaut :
// cette fonction ne fait QUE choisir LAQUELLE des 4 causes de suspension
// nommer — elle ne sait rien de l'arbitrage entre une cause de GARDE CHIMIQUE
// et une cause de SUSPENSION, qui vit entièrement dans la coquille
// (postSuspendCause, pump_controller.cpp). D'où ce test dédié.
//
// MUTATION QUE CE LOT DOIT TUER
// ------------------------------
// Retirer la ligne `if (_refusalCauseFromGuard[pumpIndex]) return;` du corps
// de `postSuspendCause()` fait échouer `test_guard_check_precedes_only_mutator`
// (le marqueur n'est plus trouvé) — et c'est EXACTEMENT la régression du
// 2026-10-18 : sans cette ligne, `logRefusalOnce()` écrase
// inconditionnellement `_refusalCauseFromGuard` à `false`, et plus rien ne
// peut reposer la cause chimique tant qu'aucun nouveau FRONT ne se présente
// (impossible pendant la suspension, cf. phase 2 ci-dessus).
//
// LIMITE CONNUE (à ne pas masquer) : extraction TEXTUELLE. Un bloc remis en
// commentaire entre les marqueurs resterait invisible pour `findMatchingBrace`
// (il tourne sur le texte déjà débarrassé de ses commentaires par
// `stripComments()`, donc un marqueur en commentaire disparaît AVANT la
// recherche — il ne peut donc pas être pris pour du code actif). Ce test ne
// prouve pas non plus que `logRefusalOnce()` lui-même reste inoffensif tant
// que la garde est propriétaire ailleurs dans le fichier : seule l'ordre
// D'APPEL depuis CE corps de fonction est vérifié. Un test on-target
// (scénario « pH bas 20h → filtration coupée 23h → reprise 9h », relevé de
// `/data` à chaque étape) fermerait ce doute — non exécutable depuis ce
// conteneur (aucun port série, cf. mémoire projet).
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Lecture des sources du projet (patron test_native_ota_order_guard /
// test_native_dose_banner_silence) — copie locale volontaire (chaque suite
// structurelle de ce dépôt est auto-suffisante, cf. commentaire d'en-tête de
// js_extract.js pour l'équivalent JS : la factorisation n'est envisagée que
// devant un TROISIÈME cas déjà dupliqué à l'identique, pas préventivement).
// -----------------------------------------------------------------------------
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

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  const char* candidates[2];

  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_dose_suspend_guard_priority/");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  candidates[0] = rooted[0] ? rooted : NULL;
  candidates[1] = rel;

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
    stripComments(buf);
    *outLen = (long)strlen(buf);
    return buf;
  }
  return NULL;
}

static int countIn(const char* hay, const char* needle) {
  int c = 0;
  const size_t n = strlen(needle);
  const size_t len = strlen(hay);
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(hay + i, needle, n) == 0) c++;
  }
  return c;
}

static long indexOf(const char* hay, const char* needle) {
  const char* p = strstr(hay, needle);
  if (p == NULL) return -1;
  return (long)(p - hay);
}

// Accolade fermante appariée à l'accolade ouvrante '{' située à `openIdx`
// (hay déjà débarrassé de ses commentaires — les chaînes restent, donc on
// continue de les sauter pour ne jamais compter une accolade littérale comme
// structurelle). -1 si non trouvée.
static long findMatchingBrace(const char* hay, long openIdx) {
  if (hay[openIdx] != '{') return -1;
  long depth = 0;
  long i = openIdx;
  while (hay[i] != '\0') {
    const char c = hay[i];
    if (c == '"') {
      i++;
      while (hay[i] != '\0' && hay[i] != '"') {
        i += (hay[i] == '\\' && hay[i + 1] != '\0') ? 2 : 1;
      }
      if (hay[i] != '\0') i++;
      continue;
    }
    if (c == '\'') {
      i++;
      if (hay[i] == '\\' && hay[i + 1] != '\0') i += 2;
      else if (hay[i] != '\0') i++;
      if (hay[i] == '\'') i++;
      continue;
    }
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) return i;
    }
    i++;
  }
  return -1;
}

// Extrait le corps { ... } (bornes incluses) de la PREMIÈRE fonction dont la
// signature exacte `signature` apparaît EXACTEMENT une fois dans `src`.
// `outBody` doit être libéré par l'appelant (free()). NULL si la signature
// n'est pas unique ou si l'accolade n'est pas appariée.
static char* extractFunctionBody(const char* src, const char* signature) {
  if (countIn(src, signature) != 1) return NULL;
  const long sigIdx = indexOf(src, signature);
  if (sigIdx < 0) return NULL;
  const long openIdx = sigIdx + (long)strlen(signature) - 1;  // '{' = dernier caractère de signature
  if (src[openIdx] != '{') return NULL;
  const long closeIdx = findMatchingBrace(src, openIdx);
  if (closeIdx < 0) return NULL;
  const size_t bodyLen = (size_t)(closeIdx - openIdx + 1);
  char* body = (char*)malloc(bodyLen + 1);
  if (body == NULL) return NULL;
  memcpy(body, src + openIdx, bodyLen);
  body[bodyLen] = '\0';
  return body;
}

// =============================================================================
// T1 (MUTATION CIBLE) — PHASE 2 : dans le corps de postSuspendCause(), la
// garde de propriété `if (_refusalCauseFromGuard[pumpIndex]) return;` doit
// précéder le SEUL mutateur du corps (`logRefusalOnce(` — point d'écriture
// de _lastRefusalCode / _lastRefusalCause / _refusalCauseFromGuard). Le corps
// ne boucle pas et ne reçoit aucun état mutable entre deux appels : un appel
// unitaire prouvé sûr vaut pour N'IMPORTE QUEL NOMBRE de ticks de suspension.
// =============================================================================
void test_guard_check_precedes_only_mutator(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/pump_controller.cpp introuvable — la garde structurelle est aveugle");

  const char* sig = "void PumpControllerClass::postSuspendCause(int pumpIndex, DoseRefusal cause) {";
  char* body = extractFunctionBody(src, sig);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      body, "postSuspendCause() introuvable ou signature non unique dans pump_controller.cpp "
            "— la garde structurelle est aveugle");

  const char* guardLine = "if (_refusalCauseFromGuard[pumpIndex]) return;";
  const char* mutator = "logRefusalOnce(";

  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(body, guardLine),
      "REGRESSION bug-172 : la garde de propriete "
      "« if (_refusalCauseFromGuard[pumpIndex]) return; » doit apparaitre "
      "EXACTEMENT une fois dans postSuspendCause() — sans elle, une cause de "
      "garde chimique posee AVANT la suspension (ex. pH sous le plancher) est "
      "ECRASEE par la premiere suspension globale (filtration coupee, "
      "etalonnage, stabilisation, assistant de debit) et ne peut plus jamais "
      "etre reaffichee tant qu'aucun nouveau FRONT de garde ne se presente");

  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(body, mutator),
      "postSuspendCause() doit appeler logRefusalOnce EXACTEMENT une fois "
      "(chemin PROPRIETAIRE impose par le gate H4) — ce test suppose un seul "
      "point de mutation dans ce corps, a reverifier si ce nombre change");

  const long guardIdx = indexOf(body, guardLine);
  const long mutatorIdx = indexOf(body, mutator);
  TEST_ASSERT_TRUE_MESSAGE(guardIdx >= 0 && mutatorIdx >= 0,
                            "marqueurs introuvables dans le corps de postSuspendCause()");
  TEST_ASSERT_TRUE_MESSAGE(
      guardIdx < mutatorIdx,
      "REGRESSION bug-172 : la garde de propriete doit PRECEDER l'appel a "
      "logRefusalOnce() dans postSuspendCause() — c'est le rang 0 de la "
      "cascade impose par le gate pool-chemistry (« une cause de garde "
      "chimique n'est jamais ecrasee »)");

  free(body);
  free(src);
}

// =============================================================================
// T2 — aucun autre mutateur direct dans le corps de postSuspendCause() ne
// peut contourner la garde de T1 (sinon T1 protégerait un chemin, laissant
// un second chemin de mutation ouvert).
// =============================================================================
void test_no_direct_mutation_bypasses_the_guard(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/pump_controller.cpp introuvable");

  const char* sig = "void PumpControllerClass::postSuspendCause(int pumpIndex, DoseRefusal cause) {";
  char* body = extractFunctionBody(src, sig);
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "postSuspendCause() introuvable ou signature non unique");

  TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, countIn(body, "_refusalCauseFromGuard[pumpIndex] ="),
      "REGRESSION bug-172 : postSuspendCause() ne doit JAMAIS ecrire "
      "_refusalCauseFromGuard directement — le seul point d'ecriture autorise "
      "est logRefusalOnce() (verifie par T1), qui est lui-meme derriere la "
      "garde de propriete");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, countIn(body, "setRefusalCode("),
      "REGRESSION bug-172 : postSuspendCause() ne doit pas appeler "
      "setRefusalCode() directement (contournerait l'edge-trigger de "
      "logRefusalOnce) — ce nombre devrait rester a 0");

  // H4 (gate pool-chemistry) : jamais la sentinelle des gardes chimiques
  // (setGuardRefusalCause) depuis ce chemin — elle est RESERVEE aux gardes,
  // un melange des deux proprietaires romprait l'arbitrage D1.
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, countIn(body, "setGuardRefusalCause("),
      "REGRESSION bug-172 (gate H4) : postSuspendCause() ne doit JAMAIS "
      "appeler setGuardRefusalCause() — la sentinelle 250 est reservee aux "
      "gardes chimiques, pas a la suspension globale");

  free(body);
  free(src);
}

// =============================================================================
// T3 — PHASE 1 (pose) : setGuardRefusalCause() prend INCONDITIONNELLEMENT la
// propriete de la cause (_refusalCauseFromGuard[pumpIndex] = true) — c'est la
// precondition sans laquelle T1 ne protegerait rien (rien n'aurait jamais
// pose le drapeau a vrai).
// =============================================================================
void test_set_guard_cause_takes_ownership(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/pump_controller.cpp introuvable");

  const char* sig = "void PumpControllerClass::setGuardRefusalCause(int pumpIndex, const String& cause) {";
  char* body = extractFunctionBody(src, sig);
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "setGuardRefusalCause() introuvable ou signature non unique");

  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(body, "_refusalCauseFromGuard[pumpIndex] = true;"),
      "setGuardRefusalCause() doit poser _refusalCauseFromGuard[pumpIndex] = true; "
      "EXACTEMENT une fois — sans cette prise de propriete, aucune garde "
      "chimique ne serait jamais protegee par T1");

  free(body);
  free(src);
}

// =============================================================================
// T4 — PHASE 3 (reprise) : clearGuardRefusalCause() est un no-op strict hors
// propriete garde — elle ne doit JAMAIS effacer une cause posee par canDose()
// (filtration arretee, quota, anti-rafale…), seulement celle d'une garde.
// =============================================================================
void test_clear_guard_cause_is_noop_outside_ownership(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/pump_controller.cpp introuvable");

  const char* sig = "void PumpControllerClass::clearGuardRefusalCause(int pumpIndex) {";
  char* body = extractFunctionBody(src, sig);
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "clearGuardRefusalCause() introuvable ou signature non unique");

  const char* guardLine = "if (!_refusalCauseFromGuard[pumpIndex]) return;";
  const char* mutator = "_refusalCauseFromGuard[pumpIndex] = false;";

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(body, guardLine),
      "clearGuardRefusalCause() doit garder l'effacement derriere "
      "« if (!_refusalCauseFromGuard[pumpIndex]) return; » EXACTEMENT une fois");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(body, mutator),
      "clearGuardRefusalCause() doit effacer _refusalCauseFromGuard EXACTEMENT une fois");

  const long guardIdx = indexOf(body, guardLine);
  const long mutatorIdx = indexOf(body, mutator);
  TEST_ASSERT_TRUE_MESSAGE(guardIdx >= 0 && mutatorIdx >= 0, "marqueurs introuvables");
  TEST_ASSERT_TRUE_MESSAGE(
      guardIdx < mutatorIdx,
      "REGRESSION : la garde de non-propriete doit PRECEDER l'effacement — "
      "sinon clearGuardRefusalCause() pourrait effacer une cause posee par "
      "canDose() (un refus REEL et toujours actif, ex. filtration arretee)");

  free(body);
  free(src);
}

// =============================================================================
// T5 — bouclage des 3 phases : la surveillance pH (tickPhGuardSurveillance(),
// seul point qui REPOSE un front de garde en phase 3) n'est atteignable
// depuis updateDosingCore() QU'EN DEHORS du bloc de suspension globale — elle
// reprend donc exactement quand la phase 2 se termine, jamais pendant.
// =============================================================================
void test_guard_surveillance_runs_outside_suspend_block(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/pump_controller.cpp introuvable");

  const char* suspendIf =
      "if (!filtrationOk || stabilizationActive || flowAssistSuspend || mqttCfg.calSessionOpen) {";
  const char* surveillanceCall = "tickPhGuardSurveillance();";

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, suspendIf),
      "le bloc de suspension globale doit apparaitre EXACTEMENT une fois dans "
      "pump_controller.cpp — sa condition a change ou a ete duplique");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, surveillanceCall),
      "tickPhGuardSurveillance(); doit etre appele EXACTEMENT une fois dans "
      "pump_controller.cpp");

  const long ifIdx = indexOf(src, suspendIf);
  TEST_ASSERT_TRUE_MESSAGE(ifIdx >= 0, "bloc de suspension introuvable");
  const long openBraceIdx = ifIdx + (long)strlen(suspendIf) - 1;
  const long closeBraceIdx = findMatchingBrace(src, openBraceIdx);
  TEST_ASSERT_TRUE_MESSAGE(closeBraceIdx >= 0,
                            "fermeture du bloc de suspension introuvable (accolade non appariee)");

  const long surveillanceIdx = indexOf(src, surveillanceCall);
  TEST_ASSERT_TRUE_MESSAGE(surveillanceIdx >= 0, "tickPhGuardSurveillance(); introuvable");

  TEST_ASSERT_TRUE_MESSAGE(
      surveillanceIdx > closeBraceIdx,
      "REGRESSION : tickPhGuardSurveillance() doit rester TEXTUELLEMENT APRES "
      "la fermeture du bloc de suspension globale — tous les chemins de ce "
      "bloc se terminant par `return;`, cet ordre garantit qu'elle ne tourne "
      "QUE quand la phase 2 est terminee (sinon la phase 3 ne pourrait jamais "
      "reposer un nouveau front de garde)");

  free(src);
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_guard_check_precedes_only_mutator);
  RUN_TEST(test_no_direct_mutation_bypasses_the_guard);
  RUN_TEST(test_set_guard_cause_takes_ownership);
  RUN_TEST(test_clear_guard_cause_is_noop_outside_ownership);
  RUN_TEST(test_guard_surveillance_runs_outside_suspend_block);
  return UNITY_END();
}
