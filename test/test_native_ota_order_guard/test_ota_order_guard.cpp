// =============================================================================
// GARDE STRUCTURELLE — feature-118 lot A (condition 1 de l'audit security-reviewer)
// =============================================================================
// Ce n'est pas un test de comportement : c'est un test STRUCTUREL qui lit le
// TEXTE des sources (patron test_native_ws_config_parity / test_native_dose_
// banner_silence — aucun harnais JS/C++ exécutable n'existe pour ces deux
// contrats). Il protège DEUX ordres impératifs, chacun documenté en commentaire
// à la source, et chacun tel qu'une inversion silencieuse ferait tomber tout
// le mécanisme d'authenticité de l'image OTA :
//
//   [G1] src/web_routes_ota.cpp : le contrôle du paramètre `sha256` (parse +
//        refus « malformée ») doit s'exécuter AVANT `g_uploadOwner = request;`.
//        Si on le déplace après, l'image a déjà été acceptée comme propriétaire
//        de la session (et donc, plus loin dans le fichier, Update.begin() a pu
//        démarrer) avant même d'avoir vérifié la forme de l'empreinte fournie —
//        c'est EXACTEMENT le défaut qualifié de GRAVE par l'audit (condition 1).
//
//   [G2] data/app.js : le champ `sha256` doit être ajouté au FormData AVANT
//        `update_file`. Le firmware (AsyncWebServer multipart) lit les champs
//        PENDANT le parsing de la partie fichier : un champ placé après le
//        fichier se lit comme ABSENT côté firmware (hasParam("sha256") rend
//        false à index==0), ce qui désarme silencieusement toute la
//        vérification — même si le code JS qui la calcule est parfaitement
//        correct.
//
//   [G3] src/web_routes_ota.cpp, feature-118 LOT B : la branche de refus
//        `OtaSha256ParamVerdict::kAbsent` (empreinte absente → upload refusé,
//        et non plus seulement journalisé comme au lot A) doit, comme la
//        branche « malformée » de G1 :
//          (a) s'exécuter AVANT `g_uploadOwner = request;` ;
//          (b) rester HORS du bloc `if (len >= 1) { ... }` (pré-contrôle de
//              l'octet magique) — ce bloc peut être vide (upload d'un fichier
//              de taille nulle, `len==0, final==true`) et le paramètre
//              `sha256` ne doit JAMAIS dépendre de la présence d'octets de
//              fichier pour être contrôlé.
//        Si (b) est violé (le refus se retrouve nesté DANS le bloc `len>=1`),
//        un upload de fichier vide contournerait le refus « absente » — le
//        chemin precis que ce lot ferme.
//
// LIMITE CONNUE (à ne pas masquer) : extraction TEXTUELLE. Un bloc remis en
// COMMENTAIRE entre les marqueurs reste invisible — la garde protège contre la
// distraction (un refactor qui déplace le bloc), pas contre un contournement
// délibéré en revue.
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Lecture des sources du projet (patron test_native_dose_banner_silence)
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
    const char* m = strstr(f, "test/test_native_ota_order_guard/");
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

// Retourne l'offset de la PREMIÈRE occurrence de `needle` dans `hay`, ou -1.
static long indexOf(const char* hay, const char* needle) {
  const char* p = strstr(hay, needle);
  if (p == NULL) return -1;
  return (long)(p - hay);
}

// Retourne l'offset de l'accolade fermante '}' correspondant à l'accolade
// ouvrante '{' située à `openIdx` dans `hay` (hay DÉJÀ débarrassé de ses
// commentaires par stripComments — les chaînes restent, donc on continue de
// les sauter ici pour ne jamais compter une accolade littérale comme
// structurelle). -1 si aucune fermeture trouvée.
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

// =============================================================================
// [G1] web_routes_ota.cpp : contrôle sha256 AVANT g_uploadOwner = request;
// =============================================================================
void test_sha256_control_precedes_upload_owner_assignment(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/web_routes_ota.cpp introuvable — la garde structurelle est aveugle");

  // Marqueur du CALCUL du verdict (parseOtaSha256Param) — doit exister UNE
  // seule fois dans le handler d'upload (index==0).
  const int callCount = countIn(src, "parseOtaSha256Param(");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, callCount,
      "parseOtaSha256Param( doit apparaitre EXACTEMENT une fois dans web_routes_ota.cpp");

  // Marqueur de l'AFFECTATION de propriété (assignation, PAS une comparaison
  // telle que `!= request` ou `request != g_uploadOwner`) — doit exister UNE
  // seule fois (la prise de propriété réelle).
  const char* ownerAssign = "g_uploadOwner = request;";
  const int ownerAssignCount = countIn(src, ownerAssign);
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, ownerAssignCount,
      "g_uploadOwner = request; doit apparaitre EXACTEMENT une fois (prise de propriété)");

  const long callIdx = indexOf(src, "parseOtaSha256Param(");
  const long ownerIdx = indexOf(src, ownerAssign);
  TEST_ASSERT_TRUE_MESSAGE(callIdx >= 0 && ownerIdx >= 0,
                            "marqueurs introuvables dans web_routes_ota.cpp");
  TEST_ASSERT_TRUE_MESSAGE(
      callIdx < ownerIdx,
      "REGRESSION feature-118 condition 1 : parseOtaSha256Param( doit precede "
      "g_uploadOwner = request; — le controle d'authenticite doit s'executer "
      "AVANT la prise de propriete de la session d'upload");

  // Le refus « malformée » (le return effectif) doit, lui aussi, précéder la
  // prise de propriété — sinon le bloc de calcul a pu être déplacé tout en
  // laissant le refus s'exécuter trop tard (ou l'inverse).
  const char* malformedCheck = "OtaSha256ParamVerdict::kMalformed)";
  const long malformedIdx = indexOf(src, malformedCheck);
  TEST_ASSERT_TRUE_MESSAGE(malformedIdx >= 0, "verification du verdict malforme introuvable");
  TEST_ASSERT_TRUE_MESSAGE(
      malformedIdx < ownerIdx,
      "REGRESSION feature-118 condition 1 : le refus « malformee » doit precede "
      "g_uploadOwner = request;");

  free(src);
}

// =============================================================================
// [G3] web_routes_ota.cpp, lot B : refus « absente » AVANT g_uploadOwner =
// request; ET HORS du bloc if (len >= 1) { ... }
// =============================================================================
void test_kabsent_branch_before_owner_and_outside_len_check(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/web_routes_ota.cpp introuvable — la garde structurelle est aveugle");

  // --- Unicité des marqueurs ------------------------------------------------
  const char* lenCheckOpen = "if (len >= 1) {";
  const char* kAbsentCond = "shaParamVerdict == OtaSha256ParamVerdict::kAbsent) {";
  const char* ownerAssign = "g_uploadOwner = request;";

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, lenCheckOpen),
      "if (len >= 1) { doit apparaitre EXACTEMENT une fois dans web_routes_ota.cpp");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, kAbsentCond),
      "la condition kAbsent doit apparaitre EXACTEMENT une fois dans web_routes_ota.cpp");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, ownerAssign),
      "g_uploadOwner = request; doit apparaitre EXACTEMENT une fois (prise de propriete)");

  const long lenCheckIdx = indexOf(src, lenCheckOpen);
  const long kAbsentIdx = indexOf(src, kAbsentCond);
  const long ownerIdx = indexOf(src, ownerAssign);
  TEST_ASSERT_TRUE_MESSAGE(lenCheckIdx >= 0 && kAbsentIdx >= 0 && ownerIdx >= 0,
                            "marqueurs introuvables dans web_routes_ota.cpp");

  // --- (a) AVANT g_uploadOwner = request; -----------------------------------
  TEST_ASSERT_TRUE_MESSAGE(
      kAbsentIdx < ownerIdx,
      "REGRESSION feature-118 lot B : le refus « absente » doit precede "
      "g_uploadOwner = request; — sinon la session est deja possedee quand le "
      "refus serait applique (owner fantome / effet de bord avant verification)");

  // --- (b) HORS du bloc if (len >= 1) { ... } -------------------------------
  // L'accolade ouvrante de lenCheckOpen est son DERNIER caractère.
  const long lenOpenBraceIdx = lenCheckIdx + (long)strlen(lenCheckOpen) - 1;
  const long lenCloseBraceIdx = findMatchingBrace(src, lenOpenBraceIdx);
  TEST_ASSERT_TRUE_MESSAGE(lenCloseBraceIdx >= 0,
                            "fermeture du bloc if (len >= 1) introuvable (brace non appariee)");
  TEST_ASSERT_TRUE_MESSAGE(
      kAbsentIdx > lenCloseBraceIdx,
      "REGRESSION feature-118 lot B : la condition kAbsent doit rester HORS du bloc "
      "if (len >= 1) { ... } — a l'interieur, un upload de fichier VIDE (len==0, "
      "final==true) contournerait le refus « absente »");

  // --- (c) markUploadRejected(request); DOIT être appelé DANS la branche ----
  // kAbsent, entre sa condition et la prise de propriété qui suit. SANS lui,
  // Update.hasError() reste FAUX côté lambda de réponse : la requête reçoit
  // "OK" et un REDÉMARRAGE PARASITE est planifié alors que l'upload a été
  // refusé (security-reviewer, feature-118 lot B — défaut qualifié de le plus
  // grave de ce lot).
  const char* markRejected = "markUploadRejected(request);";
  bool markRejectedInsideKAbsentBranch = false;
  {
    const char* search = src + kAbsentIdx;
    while (true) {
      const char* hit = strstr(search, markRejected);
      if (hit == NULL) break;
      const long hitIdx = (long)(hit - src);
      if (hitIdx >= ownerIdx) break;  // dépassé la branche kAbsent, on arrête
      markRejectedInsideKAbsentBranch = true;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(
      markRejectedInsideKAbsentBranch,
      "REGRESSION feature-118 lot B : markUploadRejected(request); doit etre appele "
      "DANS la branche kAbsent (entre sa condition et g_uploadOwner = request;) — "
      "sans lui, Update.hasError() reste FAUX et la reponse \"OK\" planifie un "
      "REDEMARRAGE PARASITE malgre le refus");

  free(src);
}

// =============================================================================
// [G4] web_server.cpp, lot B : serveStatic("/") pose EXPLICITEMENT
// .setCacheControl("no-cache") — seul endroit du firmware qui répond un 304
// (AsyncStaticWebHandler teste If-None-Match), donc le seul chemin où une
// page web mise en cache AVANT un déploiement pourrait survivre sans
// revalidation si cet en-tête disparaissait.
// =============================================================================
void test_serve_static_sets_no_cache_control(void) {
  long len = 0;
  char* src = readProjectFile("src/web_server.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/web_server.cpp introuvable — la garde structurelle est aveugle");

  const char* serveStaticCall = "serveStatic(\"/\", LittleFS, \"/\")";
  const char* cacheControlCall = ".setCacheControl(\"no-cache\")";

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, serveStaticCall),
      "serveStatic(\"/\", LittleFS, \"/\") doit apparaitre EXACTEMENT une fois dans web_server.cpp");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, cacheControlCall),
      "REGRESSION feature-118 lot B : .setCacheControl(\"no-cache\") doit apparaitre "
      "EXACTEMENT une fois (code, hors commentaire) — absent, une page en cache ne "
      "revaliderait plus apres un deploiement");

  const long serveStaticIdx = indexOf(src, serveStaticCall);
  const long cacheControlIdx = indexOf(src, cacheControlCall);
  TEST_ASSERT_TRUE_MESSAGE(serveStaticIdx >= 0 && cacheControlIdx >= 0,
                            "marqueurs introuvables dans web_server.cpp");

  // .setCacheControl(...) doit être chaîné sur CE serveStatic (donc situé
  // juste après lui, avant le point-virgule qui termine l'instruction
  // chaînée — recherché depuis serveStaticCall pour borner la fenêtre).
  const char* endOfChain = ";";
  const char* afterServeStatic = src + serveStaticIdx;
  const char* semicolon = strstr(afterServeStatic, endOfChain);
  TEST_ASSERT_NOT_NULL_MESSAGE(semicolon, "instruction serveStatic(...) non terminee par ';'");
  const long semicolonIdx = (long)(semicolon - src);

  TEST_ASSERT_TRUE_MESSAGE(
      cacheControlIdx > serveStaticIdx && cacheControlIdx < semicolonIdx,
      "REGRESSION feature-118 lot B : .setCacheControl(\"no-cache\") doit etre "
      "CHAINE sur ce serveStatic(...) (entre l'appel et le ';' qui termine "
      "l'instruction), pas ailleurs dans le fichier");

  free(src);
}

// =============================================================================
// [G2] data/app.js : sha256 ajouté au FormData AVANT update_file
// =============================================================================
void test_formdata_sha256_precedes_update_file(void) {
  long len = 0;
  char* src = readProjectFile("data/app.js", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "data/app.js introuvable — la garde structurelle est aveugle");

  const char* shaAppend = "formData.append(\"sha256\"";
  const char* fileAppend = "formData.append(\"update_file\"";

  const int shaCount = countIn(src, shaAppend);
  const int fileCount = countIn(src, fileAppend);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, shaCount,
      "formData.append(\"sha256\" doit apparaitre EXACTEMENT une fois dans data/app.js");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, fileCount,
      "formData.append(\"update_file\" doit apparaitre EXACTEMENT une fois dans data/app.js");

  const long shaIdx = indexOf(src, shaAppend);
  const long fileIdx = indexOf(src, fileAppend);
  TEST_ASSERT_TRUE_MESSAGE(shaIdx >= 0 && fileIdx >= 0, "marqueurs introuvables dans data/app.js");
  TEST_ASSERT_TRUE_MESSAGE(
      shaIdx < fileIdx,
      "REGRESSION feature-118 : formData.append(\"sha256\", ...) doit precede "
      "formData.append(\"update_file\", ...) — un champ place APRES le fichier se lit "
      "comme ABSENT cote firmware (AsyncWebServer multipart)");

  free(src);
}

// =============================================================================
// [G5] web_routes_ota.cpp, feature-151 D1 (revue de sécurité du 2026-10-18,
// condition B2) : l'appel d'authenticité de /update (computeOtaAuthenticityVerdict
// invoqué avec le littéral "/update") doit précéder (a) la prise de propriété
// de la session (`g_uploadOwner = request;`) ET (b) le pré-contrôle de l'octet
// magique (`checkOtaFirstByte(`).
// =============================================================================
// (Numérotation G5/G6, pas G4/G5 comme demandé en revue : un [G4] existe déjà
// dans ce même fichier depuis feature-118 lot B — web_server.cpp, cache
// control — et ne correspond PAS au marqueur feature-151 décrit ici. Garder
// deux [G4] différents dans le même fichier aurait été plus trompeur que de
// renuméroter. Même patron d'extraction textuelle, même esprit.)
//
// SCÉNARIO PROTÉGÉ : il y a toute la campagne D2 entre D1 et D3. D1 ne refuse
// rien (aucun comportement observable ne casse si l'ordre est inversé), donc
// un refactor qui déplacerait ce bloc après la prise de propriété ou après le
// pré-contrôle magique passerait TOUTES les suites existantes — et D3
// grefferait alors son `if` de refus au mauvais endroit (après que l'image a
// déjà été acceptée comme propriétaire de la session, voire déjà commencé à
// être flashée).
// =============================================================================
void test_update_authenticity_call_precedes_owner_and_magic_check(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/web_routes_ota.cpp introuvable — la garde structurelle est aveugle");

  const char* authCall = "logOtaAuthenticityVerdict(\"/update\", authenticity);";
  const char* ownerAssign = "g_uploadOwner = request;";
  const char* magicCheck = "checkOtaFirstByte(";

  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(src, authCall),
      "l'appel d'authenticite de /update doit apparaitre EXACTEMENT une fois");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, ownerAssign),
      "g_uploadOwner = request; doit apparaitre EXACTEMENT une fois (prise de propriete)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, magicCheck),
      "checkOtaFirstByte( doit apparaitre EXACTEMENT une fois (pre-controle de l'octet magique)");

  const long authIdx = indexOf(src, authCall);
  const long ownerIdx = indexOf(src, ownerAssign);
  const long magicIdx = indexOf(src, magicCheck);
  TEST_ASSERT_TRUE_MESSAGE(authIdx >= 0 && ownerIdx >= 0 && magicIdx >= 0,
                            "marqueurs introuvables dans web_routes_ota.cpp");

  TEST_ASSERT_TRUE_MESSAGE(
      authIdx < magicIdx,
      "REGRESSION feature-151 D1 (condition B2) : l'appel d'authenticite de "
      "/update doit precede checkOtaFirstByte( — place DEFINITIVE pour D3, "
      "Q0 du verdict security-reviewer P1");
  TEST_ASSERT_TRUE_MESSAGE(
      authIdx < ownerIdx,
      "REGRESSION feature-151 D1 (condition B2) : l'appel d'authenticite de "
      "/update doit precede g_uploadOwner = request;");

  free(src);
}

// =============================================================================
// [G6] web_routes_ota.cpp, feature-151 D1 (revue de sécurité du 2026-10-18,
// condition B2) : l'appel d'authenticité de /download-update doit précéder
// l'appel à refuseIfTlsGateClosed(request, "/download-update" — la garde
// PSRAM/heap avant la session TLS sortante.
// =============================================================================
void test_download_authenticity_call_precedes_tls_gate(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/web_routes_ota.cpp introuvable — la garde structurelle est aveugle");

  const char* authCall = "logOtaAuthenticityVerdict(\"/download-update\", authenticity);";
  const char* tlsGateCall = "refuseIfTlsGateClosed(request, \"/download-update\"";

  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(src, authCall),
      "l'appel d'authenticite de /download-update doit apparaitre EXACTEMENT une fois");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countIn(src, tlsGateCall),
      "l'appel refuseIfTlsGateClosed(.../download-update) doit apparaitre EXACTEMENT une fois");

  const long authIdx = indexOf(src, authCall);
  const long tlsGateIdx = indexOf(src, tlsGateCall);
  TEST_ASSERT_TRUE_MESSAGE(authIdx >= 0 && tlsGateIdx >= 0,
                            "marqueurs introuvables dans web_routes_ota.cpp");

  TEST_ASSERT_TRUE_MESSAGE(
      authIdx < tlsGateIdx,
      "REGRESSION feature-151 D1 (condition B2) : l'appel d'authenticite de "
      "/download-update doit precede refuseIfTlsGateClosed(...), AVANT tout "
      "esp_partition_erase_range()/LittleFS.end() — place DEFINITIVE pour D3");

  free(src);
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_sha256_control_precedes_upload_owner_assignment);
  RUN_TEST(test_kabsent_branch_before_owner_and_outside_len_check);
  RUN_TEST(test_serve_static_sets_no_cache_control);
  RUN_TEST(test_formdata_sha256_precedes_update_file);
  RUN_TEST(test_update_authenticity_call_precedes_owner_and_magic_check);
  RUN_TEST(test_download_authenticity_call_precedes_tls_gate);
  return UNITY_END();
}
