// =============================================================================
// GARDE STRUCTURELLE — feature-118 lot B, fail-closed de ota_update.sh
// =============================================================================
// ota_update.sh n'est pas du C++/JS : il n'existe aucun patron de test natif
// EXÉCUTANT le script (ni interpréteur bash embarqué dans ce dépôt de test,
// ni mock de `curl`/`sha256sum`). Le comportement fail-closed a déjà été
// PROUVÉ par exécution manuelle (PATH privé de sha256sum/shasum, curl
// remplacé par un piège ⇒ exit 1, marqueur du piège absent — security-
// reviewer, feature-118 lot B, T4). Ce test ne re-démontre PAS ce
// comportement : il le VERROUILLE contre une régression future, par lecture
// STRUCTURELLE du texte du script — même patron que test_native_ota_order_
// guard (G1/G3), transposé à la syntaxe de commentaire bash (`#`, pas
// `//`/`/* */`).
//
// CE QUE CE TEST PROTÈGE
//   [F1] Dans update_file(), l'échec de compute_sha256() (`if ! digest=$(...)
//        ; then ... return 1; fi`) doit s'exécuter AVANT l'appel curl réel
//        (`curl -s --show-error ...`) — sinon un envoi sans empreinte
//        atteindrait le réseau avant que le script ne refuse.
//   [F2] Le corps de compute_sha256() ne doit comporter AUCUNE troisième
//        branche de repli : seuls `sha256sum`/`shasum` produisent un
//        résultat, toute autre voie est "return 1" — jamais un `echo` qui
//        produirait un digest bidon (qui serait silencieusement accepté par
//        `-F "sha256=sha256:${digest}"`, fail-OPEN invisible).
//
// LIMITE CONNUE (identique à test_native_ota_order_guard) : extraction
// TEXTUELLE. Un bloc remis en commentaire entre les marqueurs resterait
// invisible — protège la distraction, pas un contournement délibéré en
// revue.
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Lecture de ota_update.sh — stripComments BASH (# jusqu'à la fin de ligne ;
// guillemets simples/doubles respectés, échappement `\` pris en compte dans
// les doubles-guillemets). Les quatre backticks présents dans les commentaires
// de ce script (citations façon markdown : `sha256sum`, `grep -q "OK"`, ...)
// vivent tous DANS des lignes `#` : ils disparaissent avec le reste du
// commentaire, aucun traitement spécial n'est nécessaire pour eux.
// -----------------------------------------------------------------------------
static void stripBashComments(char* s) {
  size_t i = 0;
  while (s[i] != '\0') {
    if (s[i] == '#') {
      while (s[i] != '\0' && s[i] != '\n') s[i++] = ' ';
    } else if (s[i] == '\'') {
      i++;
      while (s[i] != '\0' && s[i] != '\'') i++;
      if (s[i] != '\0') i++;
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
    const char* m = strstr(f, "test/test_native_ota_update_fail_closed/");
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
    stripBashComments(buf);
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

// =============================================================================
// [F1] échec de compute_sha256() AVANT l'appel curl réel
// =============================================================================
void test_sha256_failure_check_precedes_curl_call(void) {
  long len = 0;
  char* src = readProjectFile("ota_update.sh", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "ota_update.sh introuvable — la garde structurelle est aveugle");

  const char* digestCheck = "if ! digest=$(compute_sha256 \"$file\"); then";
  const char* curlCall = "curl -s --show-error --connect-timeout 10 --max-time 120";

  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(src, digestCheck),
      "le controle d'echec de compute_sha256 doit apparaitre EXACTEMENT une fois");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(src, curlCall),
      "l'appel curl reel doit apparaitre EXACTEMENT une fois dans ota_update.sh");

  const long digestIdx = indexOf(src, digestCheck);
  const long curlIdx = indexOf(src, curlCall);
  TEST_ASSERT_TRUE_MESSAGE(digestIdx >= 0 && curlIdx >= 0, "marqueurs introuvables dans ota_update.sh");
  TEST_ASSERT_TRUE_MESSAGE(
      digestIdx < curlIdx,
      "REGRESSION feature-118 lot B : le controle d'echec de compute_sha256 doit "
      "precede l'appel curl reel — sinon un envoi SANS empreinte atteindrait le "
      "reseau avant le refus fail-closed");

  free(src);
}

// =============================================================================
// [F2] compute_sha256() n'a pas de troisième voie : "return 1" seul, jamais
// un echo de repli qui produirait un digest bidon
// =============================================================================
void test_compute_sha256_else_branch_is_return_1_only(void) {
  long len = 0;
  char* src = readProjectFile("ota_update.sh", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "ota_update.sh introuvable — la garde structurelle est aveugle");

  // Bloc exact attendu : la branche "else" de compute_sha256() se limite à
  // "return 1" — ni echo, ni valeur par défaut, avant la fermeture de la
  // fonction (`fi` puis `}`).
  const char* elseReturnBlock = "else\n        return 1\n    fi\n}";
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(src, elseReturnBlock),
      "la branche else de compute_sha256() doit etre EXACTEMENT \"return 1\" "
      "(aucun repli silencieux vers un digest bidon)");

  free(src);
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_sha256_failure_check_precedes_curl_call);
  RUN_TEST(test_compute_sha256_else_branch_is_return_1_only);
  return UNITY_END();
}
