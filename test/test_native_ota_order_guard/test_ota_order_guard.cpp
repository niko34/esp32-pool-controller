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
#include <ctype.h>

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

  const char* authCall = "logOtaAuthenticityVerdict(\"/update\", authenticity, switchDecision);";
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

  const char* authCall = "logOtaAuthenticityVerdict(\"/download-update\", authenticity, switchDecision);";
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

// =============================================================================
// [G7] web_routes_ota.cpp, feature-151 D3-a : dans /update, la RÉSOLUTION DU
// GENRE (resolveOtaEffectiveKind + réaffectation de `cmd`) précède l'octet
// magique, g_uploadIsFilesystem, LittleFS.end() et Update.begin(). Sinon le
// genre signé n'aurait aucun effet sur le choix de partition/contrôle magique
// — ou pire, s'appliquerait APRÈS que la partition a été choisie/démontée.
// =============================================================================
void test_kind_resolution_precedes_magic_and_partition_effects(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/web_routes_ota.cpp introuvable — la garde structurelle est aveugle");

  const char* resolveCall = "resolveOtaEffectiveKind(";
  const char* cmdAssign = "cmd = (kindResolution.effectiveKind == OtaManifestKind::kFilesystem)";
  const char* magic = "checkOtaFirstByte(";
  const char* isFsAssign = "g_uploadIsFilesystem = (cmd == U_SPIFFS);";
  const char* fsEnd = "LittleFS.end();";
  const char* updBegin = "Update.begin(UPDATE_SIZE_UNKNOWN, cmd)";

  // Deux sites de résolution exactement : /update et /download-update.
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(src, resolveCall),
      "resolveOtaEffectiveKind( doit apparaitre EXACTEMENT 2 fois (/update et /download-update)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, cmdAssign),
      "la reaffectation de cmd par le genre effectif doit apparaitre EXACTEMENT une fois");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, magic), "checkOtaFirstByte( doit etre unique");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, isFsAssign), "g_uploadIsFilesystem = (cmd == U_SPIFFS); doit etre unique");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, updBegin), "Update.begin(UPDATE_SIZE_UNKNOWN, cmd) doit etre unique");

  // Le 1er site (ordre du fichier) est celui de /update.
  const long resolveIdx = indexOf(src, resolveCall);
  const long cmdIdx = indexOf(src, cmdAssign);
  const long magicIdx = indexOf(src, magic);
  const long isFsIdx = indexOf(src, isFsAssign);
  const long updIdx = indexOf(src, updBegin);
  const long endIdx = indexOf(src, fsEnd);  // 1re occurrence = chemin /update
  TEST_ASSERT_TRUE_MESSAGE(resolveIdx >= 0 && cmdIdx >= 0 && magicIdx >= 0 && isFsIdx >= 0 &&
                               updIdx >= 0 && endIdx >= 0,
                           "marqueurs introuvables dans web_routes_ota.cpp");

  TEST_ASSERT_TRUE_MESSAGE(resolveIdx < cmdIdx,
      "REGRESSION feature-151 D3-a : la resolution doit preceder la reaffectation de cmd");
  TEST_ASSERT_TRUE_MESSAGE(cmdIdx < magicIdx,
      "REGRESSION feature-151 D3-a : le genre effectif (cmd) doit etre resolu AVANT "
      "checkOtaFirstByte( — sinon l'octet magique est controle contre le mauvais genre");
  TEST_ASSERT_TRUE_MESSAGE(cmdIdx < isFsIdx,
      "REGRESSION feature-151 D3-a : le genre doit etre resolu AVANT g_uploadIsFilesystem");
  TEST_ASSERT_TRUE_MESSAGE(cmdIdx < endIdx,
      "REGRESSION feature-151 D3-a : le genre doit etre resolu AVANT LittleFS.end()");
  TEST_ASSERT_TRUE_MESSAGE(cmdIdx < updIdx,
      "REGRESSION feature-151 D3-a : le genre doit etre resolu AVANT Update.begin()");

  free(src);
}

// =============================================================================
// [G8] web_routes_ota.cpp, feature-151 D3-a : dans /download-update, le genre
// EFFECTIF (isFilesystem réaffecté) précède le CHOIX DE PARTITION
// (`if (isFilesystem) {`, première occurrence = branche de sélection).
// =============================================================================
void test_download_kind_resolution_precedes_partition_choice(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      src, "src/web_routes_ota.cpp introuvable — la garde structurelle est aveugle");

  const char* effective = "isFilesystem = (kindResolution.effectiveKind == OtaManifestKind::kFilesystem);";
  const char* choice = "if (isFilesystem) {";
  const char* tlsGate = "refuseIfTlsGateClosed(request, \"/download-update\"";

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, effective),
      "la reaffectation de isFilesystem par le genre effectif doit apparaitre EXACTEMENT une fois");
  TEST_ASSERT_TRUE_MESSAGE(countIn(src, choice) >= 1,
      "le choix de partition `if (isFilesystem) {` est introuvable");

  const long effIdx = indexOf(src, effective);
  const long choiceIdx = indexOf(src, choice);
  const long gateIdx = indexOf(src, tlsGate);
  TEST_ASSERT_TRUE_MESSAGE(effIdx >= 0 && choiceIdx >= 0 && gateIdx >= 0, "marqueurs introuvables");
  TEST_ASSERT_TRUE_MESSAGE(effIdx < choiceIdx,
      "REGRESSION feature-151 D3-a : le genre effectif doit preceder le CHOIX DE PARTITION "
      "dans /download-update");
  TEST_ASSERT_TRUE_MESSAGE(effIdx < gateIdx,
      "REGRESSION feature-151 D3-a : le genre effectif doit preceder la garde TLS");

  free(src);
}

// =============================================================================
// [G9] data/app.js, bug-176 (ex feature-151 D3-a) : INTENTION INCHANGEE — chaque
// corps /download-update porte manifeste ET signature. Les parametres viennent
// desormais de `await fetchSigParams(<genre>, ...)` (GET /ota-sig?kind=), appele
// JUSTE AVANT chaque POST, SEQUENTIELLEMENT. Proprietes figees :
//   1. chaque corps porte la variable de signature ;
//   2. cette variable vient d'un await fetchSigParams AVANT le POST ;
//   3. fetchSigParams retourne sigParams(), qui reste tout-ou-rien ;
//   4. LE GENRE CONCORDE : corps filesystem <- signature filesystem, firmware <- firmware ;
//   5. aucun Promise.all sur /ota-sig (handlers serialises cote firmware).
// =============================================================================
static char* sliceCopy(const char* src, long from, long to) {
  char* out = (char*)malloc((size_t)(to - from) + 2);
  if (!out) return NULL;
  memcpy(out, src + from, (size_t)(to - from) + 1);
  out[to - from + 1] = '\0';
  return out;
}

// Extrait l'identifiant `X` de `const X = await fetchSigParams("<kind>"`.
static bool sigVarForKind(const char* src, const char* kind, char* name, size_t cap, long* atIdx) {
  char needle[96];
  snprintf(needle, sizeof(needle), " = await fetchSigParams(\"%s\"", kind);
  if (countIn(src, needle) != 1) return false;
  const long idx = indexOf(src, needle);
  long b = idx;
  while (b > 0 && (isalnum((unsigned char)src[b - 1]) || src[b - 1] == '_')) --b;
  const size_t n = (size_t)(idx - b);
  if (n == 0 || n + 1 > cap) return false;
  memcpy(name, src + b, n);
  name[n] = '\0';
  *atIdx = idx;
  return true;
}

// Le corps contient-il `+ <name> +` (variable passee comme terme de concatenation) ?
static bool bodyUsesVar(const char* body, const char* name) {
  char pat[96];
  snprintf(pat, sizeof(pat), "+ %s +", name);
  if (strstr(body, pat) != NULL) return true;
  snprintf(pat, sizeof(pat), "%s +", name);
  const char* p = strstr(body, pat);
  while (p) {
    if (p == body || !(isalnum((unsigned char)p[-1]) || p[-1] == '_')) return true;
    p = strstr(p + 1, pat);
  }
  return false;
}

void test_download_bodies_carry_manifest_and_signature(void) {
  long len = 0;
  char* src = readProjectFile("data/app.js", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "data/app.js introuvable — la garde structurelle est aveugle");

  const char* callOpen = "authFetch(\"/download-update\", {";
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(src, callOpen),
      "authFetch(\"/download-update\", { doit apparaitre EXACTEMENT 2 fois (filesystem + firmware)");

  // (2) + (4) : une variable de signature par genre, issue du bon genre.
  char fsVar[48], fwVar[48];
  long fsAt = -1, fwAt = -1;
  TEST_ASSERT_TRUE_MESSAGE(sigVarForKind(src, "filesystem", fsVar, sizeof(fsVar), &fsAt),
      "REGRESSION bug-176 : `X = await fetchSigParams(\"filesystem\"` doit apparaitre EXACTEMENT une fois");
  TEST_ASSERT_TRUE_MESSAGE(sigVarForKind(src, "firmware", fwVar, sizeof(fwVar), &fwAt),
      "REGRESSION bug-176 : `X = await fetchSigParams(\"firmware\"` doit apparaitre EXACTEMENT une fois");
  TEST_ASSERT_TRUE_MESSAGE(strcmp(fsVar, fwVar) != 0,
      "REGRESSION bug-176 : les deux genres partagent la meme variable de signature");

  const char* from = src;
  for (int k = 0; k < 2; ++k) {
    const char* hit = strstr(from, callOpen);
    TEST_ASSERT_NOT_NULL(hit);
    const long callIdx = (long)(hit - src);
    const long openIdx = callIdx + (long)strlen(callOpen) - 1;
    const long closeIdx = findMatchingBrace(src, openIdx);
    TEST_ASSERT_TRUE_MESSAGE(closeIdx > openIdx, "corps authFetch(/download-update) non referme");
    char* body = sliceCopy(src, openIdx, closeIdx);
    TEST_ASSERT_NOT_NULL(body);

    const bool isFsBody = (k == 0);
    const char* mine = isFsBody ? fsVar : fwVar;
    const char* other = isFsBody ? fwVar : fsVar;
    const long mineAt = isFsBody ? fsAt : fwAt;
    // le corps est bien celui du genre attendu (URL du bon artefact)
    TEST_ASSERT_TRUE_MESSAGE(strstr(body, isFsBody ? "filesystem_url" : "firmware_url") != NULL,
        "REGRESSION bug-176 : l'ordre des POST n'est plus filesystem puis firmware");
    TEST_ASSERT_TRUE_MESSAGE(bodyUsesVar(body, mine),
        "REGRESSION bug-176 : un corps /download-update ne porte plus la signature de SON genre "
        "(manifeste + signature issus de fetchSigParams)");
    TEST_ASSERT_FALSE_MESSAGE(bodyUsesVar(body, other),
        "REGRESSION bug-176 : un corps /download-update porte la signature de l'AUTRE genre "
        "(inversion filesystem/firmware : la verification d'authenticite echouerait ou serait faussee)");
    TEST_ASSERT_TRUE_MESSAGE(mineAt < callIdx,
        "REGRESSION bug-176 : la signature doit etre demandee (await fetchSigParams) AVANT le POST");
    free(body);
    from = hit + strlen(callOpen);
  }
  // le firmware est demande apres le POST filesystem (sequence, pas de parallelisme)
  TEST_ASSERT_TRUE_MESSAGE(fsAt < fwAt, "REGRESSION bug-176 : fetchSigParams filesystem doit preceder firmware");

  // (3) fetchSigParams : tout-ou-rien via sigParams(), genre transmis a /ota-sig.
  const char* fetchDef = "async function fetchSigParams(kind, latestVersion) {";
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, fetchDef), "async function fetchSigParams doit etre unique");
  const long fIdx = indexOf(src, fetchDef);
  const long fClose = findMatchingBrace(src, fIdx + (long)strlen(fetchDef) - 1);
  TEST_ASSERT_TRUE(fClose > fIdx);
  char* fdef = sliceCopy(src, fIdx, fClose);
  TEST_ASSERT_NOT_NULL(fdef);
  TEST_ASSERT_TRUE_MESSAGE(strstr(fdef, "return sigParams(d.manifest, d.signature);") != NULL,
      "REGRESSION bug-176 : fetchSigParams doit construire son retour avec sigParams(d.manifest, d.signature)");
  TEST_ASSERT_TRUE_MESSAGE(strstr(fdef, "\"/ota-sig?kind=\" + encodeURIComponent(kind)") != NULL,
      "REGRESSION bug-176 : fetchSigParams doit interroger /ota-sig avec SON parametre kind");
  TEST_ASSERT_TRUE_MESSAGE(strstr(fdef, "&manifest=") == NULL && strstr(fdef, "&signature=") == NULL,
      "REGRESSION bug-176 : fetchSigParams ne doit pas fabriquer lui-meme les champs (tout-ou-rien = sigParams)");
  free(fdef);

  const char* sigDef = "function sigParams(manifest, signature) {";
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, sigDef), "function sigParams doit etre unique");
  const long defIdx = indexOf(src, sigDef);
  const long defClose = findMatchingBrace(src, defIdx + (long)strlen(sigDef) - 1);
  TEST_ASSERT_TRUE(defClose > defIdx);
  char* def = sliceCopy(src, defIdx, defClose);
  TEST_ASSERT_NOT_NULL(def);
  TEST_ASSERT_TRUE_MESSAGE(strstr(def, "&manifest=") != NULL, "sigParams() n'emet plus le champ manifest");
  TEST_ASSERT_TRUE_MESSAGE(strstr(def, "&signature=") != NULL, "sigParams() n'emet plus le champ signature");
  free(def);

  // (5) aucun Promise.all (hors commentaire) dont l'argument touche /ota-sig.
  const char* p = strstr(src, "Promise.all");
  while (p) {
    const char* ls = p;
    while (ls > src && ls[-1] != '\n') --ls;
    bool inComment = false;
    for (const char* q = ls; q + 1 < p; ++q) if (q[0] == '/' && q[1] == '/') { inComment = true; break; }
    if (!inComment) {
      char win[601];
      strncpy(win, p, 600);
      win[600] = '\0';
      TEST_ASSERT_TRUE_MESSAGE(strstr(win, "fetchSigParams") == NULL && strstr(win, "ota-sig") == NULL,
          "REGRESSION bug-176 : Promise.all sur /ota-sig — les handlers firmware sont serialises sur une "
          "tache unique, deux connexions simultanees reconstruisent le defaut corrige (relais saute)");
    }
    p = strstr(p + 1, "Promise.all");
  }

  free(src);
}


// =============================================================================
// [G7] feature-151 D3-b2 (C8) + INVARIANT DU LOT : « les gardes sont INERTES ».
// Le lot ne doit ni refuser ni accepter quoi que ce soit de neuf. Propriétés figées :
//   1. exactement 2 LECTURES (otaSignatureSwitchRead) dans web_routes_ota.cpp, chacune AVANT le
//      premier Update.begin() de sa fonction ;
//   2. exactement 3 CONSOMMATIONS, chacune APRÈS le point de succès de sa fonction
//      (Update.end(true) ; pour le FS de /download-update : après le contrôle d'empreinte et
//      la levée du marqueur) — le 3ᵉ site est celui qu'on oublie ;
//   3. la décision n'est JAMAIS une CONDITION : aucune ligne qui mentionne switchDecision /
//      switchWaived / g_uploadSwitchWaived ne contient if / return / while / && / || / ? ;
//   4. la décision capturée n'est écrite qu'UNE fois par site (à index==0 / à l'entrée) et
//      jamais relue pour accorder (C8) ;
//   5. le verdict d'authenticité reste déterminé par otaAuthenticityIsRejected seul (C4) ;
//   6. aucune mention de otaSignatureSwitch dans auth.* ni ota_signature.* (C8/C11) ;
//   7. les routes de l'interrupteur sont enregistrées en AsyncURIMatcher::exact (C2).
// =============================================================================
static long nthIndexOf(const char* hay, const char* needle, int n) {
  const char* p = hay;
  for (int i = 0; i < n; ++i) {
    p = strstr(p, needle);
    if (p == NULL) return -1;
    if (i < n - 1) p += strlen(needle);
  }
  return (long)(p - hay);
}

// Corps (offset ouvrant/fermant) de la fonction dont la signature commence par `sig`.
static bool functionBody(const char* src, const char* sig, long* open, long* close) {
  const long at = indexOf(src, sig);
  if (at < 0) return false;
  const char* b = strchr(src + at, '{');
  if (b == NULL) return false;
  *open = (long)(b - src);
  *close = findMatchingBrace(src, *open);
  return *close > *open;
}

void test_d3b2_lectures_et_consommations_comptees_et_ordonnees(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/web_routes_ota.cpp introuvable — garde aveugle");

  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(src, "otaSignatureSwitchRead("),
      "D3-b2 : EXACTEMENT 2 lectures de l'interrupteur (/update et /download-update)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, countIn(src, "otaSignatureSwitchConsume("),
      "D3-b2 (C8) : EXACTEMENT 3 sites de consommation (upload, FS, firmware de /download-update) — "
      "le 3e est celui qu'on oublie");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, "otaSignatureSwitchArm("),
      "D3-b2 (C6) : l'armement n'est PAS appelable depuis un chemin d'envoi d'image");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, "otaSignatureSwitchPeek("),
      "D3-b2 (C6) : Peek est réservé à mqtt_manager.cpp");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, "otaSignatureSwitchClear("),
      "D3-b2 : seule la consommation désarme depuis un envoi d'image");

  long uo = 0, uc = 0, do_ = 0, dc = 0;
  TEST_ASSERT_TRUE(functionBody(src, "static void handleOtaUpdate(AsyncWebServerRequest* request, const String& filename", &uo, &uc));
  TEST_ASSERT_TRUE(functionBody(src, "static void handleDownloadUpdate(AsyncWebServerRequest* request) {", &do_, &dc));

  // --- /update (handleOtaUpdate) : 1 lecture, 1 consommation
  char* up = sliceCopy(src, uo, uc);
  TEST_ASSERT_EQUAL_INT(1, countIn(up, "otaSignatureSwitchRead("));
  TEST_ASSERT_EQUAL_INT(1, countIn(up, "otaSignatureSwitchConsume("));
  const long uRead = indexOf(up, "otaSignatureSwitchRead(");
  const long uBegin = indexOf(up, "Update.begin(");
  const long uEnd = indexOf(up, "if (Update.end(true))");
  const long uCons = indexOf(up, "otaSignatureSwitchConsume(");
  TEST_ASSERT_TRUE_MESSAGE(uRead >= 0 && uBegin >= 0 && uEnd >= 0 && uCons >= 0, "marqueurs /update");
  TEST_ASSERT_TRUE_MESSAGE(uRead < uBegin, "REGRESSION D3-b2 : la lecture doit PRECEDER Update.begin (/update)");
  TEST_ASSERT_TRUE_MESSAGE(uEnd < uCons, "REGRESSION D3-b2 : la consommation doit SUIVRE Update.end(true) (/update)");
  // La consommation est dans la branche de SUCCES : entre `if (Update.end(true)) {` et son `else`.
  const long succOpen = uEnd + (long)strlen("if (Update.end(true))") ;
  const char* br = strchr(up + succOpen, '{');
  TEST_ASSERT_NOT_NULL(br);
  const long brOpen = (long)(br - up);
  const long brClose = findMatchingBrace(up, brOpen);
  TEST_ASSERT_TRUE_MESSAGE(uCons > brOpen && uCons < brClose,
      "REGRESSION D3-b2 : la consommation (/update) doit etre DANS la branche de succes de Update.end(true)");
  free(up);

  // --- /download-update : 1 lecture, 2 consommations
  char* dl = sliceCopy(src, do_, dc);
  TEST_ASSERT_EQUAL_INT(1, countIn(dl, "otaSignatureSwitchRead("));
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(dl, "otaSignatureSwitchConsume("),
      "/download-update : 2 consommations (filesystem ET firmware)");
  const long dRead = indexOf(dl, "otaSignatureSwitchRead(");
  const long dBegin = indexOf(dl, "Update.begin(");
  const long c1 = nthIndexOf(dl, "otaSignatureSwitchConsume(", 1);
  const long c2 = nthIndexOf(dl, "otaSignatureSwitchConsume(", 2);
  const long dEnd = indexOf(dl, "if (Update.end(true))");
  const long fsMismatch = indexOf(dl, "image filesystem corrompue ou tronquée");
  const long fsMarker = indexOf(dl, "OtaRecoveryUpdatePhase::kSucceeded");
  TEST_ASSERT_TRUE_MESSAGE(dRead >= 0 && dBegin >= 0 && c1 >= 0 && c2 >= 0 && dEnd >= 0 && fsMismatch >= 0 && fsMarker >= 0,
      "marqueurs /download-update");
  TEST_ASSERT_TRUE_MESSAGE(dRead < dBegin, "REGRESSION D3-b2 : la lecture doit PRECEDER Update.begin (/download-update)");
  TEST_ASSERT_TRUE_MESSAGE(dRead < fsMismatch, "REGRESSION D3-b2 : la lecture doit preceder le chemin filesystem");
  // Site FS : après le contrôle d'empreinte (qui peut `return`) ET la levée du marqueur.
  TEST_ASSERT_TRUE_MESSAGE(c1 > fsMismatch && c1 > fsMarker,
      "REGRESSION D3-b2 : consommation FS apres le controle d'empreinte et la levee du marqueur");
  // Site firmware : le 3e. DANS la branche de succès de Update.end(true).
  TEST_ASSERT_TRUE_MESSAGE(c2 > dEnd, "REGRESSION D3-b2 : consommation firmware apres Update.end(true) (3e site)");
  const char* fb = strchr(dl + dEnd, '{');
  const long fbOpen = (long)(fb - dl);
  const long fbClose = findMatchingBrace(dl, fbOpen);
  TEST_ASSERT_TRUE_MESSAGE(c2 > fbOpen && c2 < fbClose,
      "REGRESSION D3-b2 : le 3e site doit etre DANS la branche de succes de Update.end(true)");
  free(dl);
  free(src);
}

// [G7, propriété 3 RÉÉCRITE par D3-c] La décision de l'autorisation temporaire (switchDecision /
// switchWaived / g_uploadSwitchWaived) est désormais UNE CONDITION — mais exactement deux, de forme
// LITTÉRALE : `refusal.rejected && !g_uploadSwitchWaived` (/update) et
// `refusal.rejected && !switchWaived` (/download-update). Tout le reste (condition C4) : AUCUNE
// ligne qui la mentionne ne touche au verdict (otaAuthenticityIsRejected, computation.verdict) ni
// au niveau de journal (systemLogger, warning, info) : la levée ne change NI le verdict NI le niveau.
void test_d3b2_la_decision_n_est_jamais_une_condition(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  const char* names[] = {"switchDecision", "switchWaived", "g_uploadSwitchWaived"};
  const char* litA = "refusal.rejected && !g_uploadSwitchWaived";
  const char* litB = "refusal.rejected && !switchWaived";
  int mentions = 0, conditionLines = 0;
  const char* line = src;
  while (*line) {
    const char* nl = strchr(line, '\n');
    const size_t n = nl ? (size_t)(nl - line) : strlen(line);
    char buf[512];
    const size_t m = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
    memcpy(buf, line, m);
    buf[m] = '\0';
    bool mentioned = false;
    for (const char* nm : names) if (strstr(buf, nm)) mentioned = true;
    if (mentioned) {
      mentions++;
      char msg[200];
      const char* verdictOrLog[] = {"otaAuthenticityIsRejected", "computation.verdict", "systemLogger",
                                    "warning", "info"};
      for (const char* f : verdictOrLog) {
        snprintf(msg, sizeof(msg), "REGRESSION D3-c (C4) : la levee touche le verdict ou le niveau (« %s ») : %.100s", f, buf);
        TEST_ASSERT_TRUE_MESSAGE(strstr(buf, f) == NULL, msg);
      }
      const char* condish[] = {"if (", "if(", "return", "while", "&&", "||", " ? ", "refuse", "failOtaUpload", "send("};
      bool isCond = false;
      for (const char* f : condish) if (strstr(buf, f)) isCond = true;
      if (isCond) {
        conditionLines++;
        snprintf(msg, sizeof(msg), "REGRESSION D3-c : condition de forme non litterale : %.100s", buf);
        TEST_ASSERT_TRUE_MESSAGE(strstr(buf, litA) != NULL || strstr(buf, litB) != NULL, msg);
      }
    }
    if (!nl) break;
    line = nl + 1;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, conditionLines, "D3-c : EXACTEMENT 2 lignes testent la decision");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, litA), "D3-c : forme litterale /update (1 occurrence)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(src, litB), "D3-c : forme litterale /download-update (1 occurrence)");
  TEST_ASSERT_TRUE_MESSAGE(mentions >= 7, "garde aveugle : la decision n'est plus mentionnee comme attendu");

  // Captures : une seule ecriture par site, jamais ailleurs (comptes INCHANGES).
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "g_uploadSwitchWaived = ("));
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "otaSignatureSwitchConsume(g_uploadSwitchWaived)"));
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "static bool g_uploadSwitchWaived = false;"));
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "g_uploadSwitchWaived = (switchDecision.requirement == OtaSignatureRequirement::kWaived)"));
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "switchWaived = (switchDecision.requirement == OtaSignatureRequirement::kWaived)"));
  TEST_ASSERT_EQUAL_INT(2, countIn(src, "otaSignatureSwitchConsume(switchWaived)"));
  free(src);
}

// [C4] le verdict reste distinct de la levée : wouldReject ne dépend que du verdict.
void test_d3b2_verdict_et_levee_distincts(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  long o = 0, c = 0;
  TEST_ASSERT_TRUE(functionBody(src, "static void logOtaAuthenticityVerdict(const char* routeName,", &o, &c));
  char* body = sliceCopy(src, o, c);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(body, "const bool wouldReject = otaAuthenticityIsRejected(computation.verdict);"),
      "C4 : wouldReject ne doit venir QUE du verdict");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(body, "if (wouldReject)"), "C4 : un seul pilote de niveau");
  const long wr = indexOf(body, "if (wouldReject)");
  const long lev = indexOf(body, "const bool armed");
  TEST_ASSERT_TRUE(wr > lev && lev >= 0);
  // `armed` / `sw` ne pilotent PAS le niveau : le bloc `if (wouldReject) {...} else {...}` n'en mentionne aucun.
  const char* ob = strchr(body + wr, '{');
  const long wOpen = (long)(ob - body);
  const long wClose = findMatchingBrace(body, wOpen);
  const char* eb = strstr(body + wClose, "else");
  TEST_ASSERT_NOT_NULL(eb);
  const long eOpen = (long)(strchr(eb, '{') - body);
  const long eClose = findMatchingBrace(body, eOpen);
  char* gate = sliceCopy(body, wOpen, eClose);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(gate, "armed") + countIn(gate, "sw.") + countIn(gate, "levee"),
      "C4 : la levee ne pilote ni le niveau ni les octets canoniques");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(gate, "canonicalLen > 0"), "C4 : octets canoniques conserves sous wouldReject");
  free(gate);
  TEST_ASSERT_EQUAL_INT(0, countIn(body, "return"));
  // D3-c : la ligne dit le verdict (libelle) ET ce qui a ete decide ; la mention de levee
  // (`levee`) s'AJOUTE en fin de ligne, elle ne remplace pas le verdict.
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(body, "authenticité OTA : %s — refusé : %s%s"),
      "C4 : la ligne porte verdict ET decision, la levee en suffixe");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(body, "otaAuthenticityVerdictLabel(computation.verdict)"),
      "C4 : le libelle du verdict reste journalise");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(body, "(wouldReject && !armed) ? \"oui\" : \"non\", levee);"),
      "C4 : « refuse » = verdict ET pas de levee ; la levee s'ajoute en suffixe");
  free(body);
  free(src);
}


// [C9 + INVARIANT] logOtaVersionAgreement ne fait que LIRE et JOURNALISER : aucun refus, aucune
// écriture d'état d'envoi, `expected_version` jamais brut dans un journal ni stocké.
void test_d3b2_liaison_version_est_purement_journalistique(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  // Définition (la 2e occurrence de la signature : la 1re est la déclaration anticipée).
  const long def = nthIndexOf(src, "static void logOtaVersionAgreement(const char* routeName, bool kindIsFilesystem,", 2);
  TEST_ASSERT_TRUE_MESSAGE(def > 0, "definition de logOtaVersionAgreement introuvable");
  const char* b = strchr(src + def, '{');
  const long open = (long)(b - src);
  const long close = findMatchingBrace(src, open);
  TEST_ASSERT_TRUE(close > open);
  char* body = sliceCopy(src, open, close);
  const char* forbidden[] = {"Update.", "failOtaUpload", "send(", "g_upload", "storeSigSource", "abort",
                             "setOtaInProgress", "g_restartRequested", "systemLogger.critical",
                             "LittleFS", "otaSignatureSwitchArm", "otaSignatureSwitchClear"};
  for (const char* f : forbidden) {
    char msg[120];
    snprintf(msg, sizeof(msg), "REGRESSION D3-b2 (inerte) : logOtaVersionAgreement contient « %s »", f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, f), msg);
  }
  // Seul `return;` (sortie silencieuse, void) est permis : aucun return avec valeur.
  TEST_ASSERT_EQUAL_INT(1, countIn(body, "return"));
  TEST_ASSERT_EQUAL_INT(1, countIn(body, "return;"));
  // La chaine brute du parametre n'est jamais concatenee au journal : on ne la lit que via param.c_str()
  // pour la SELECTION ; le journal est re-serialise depuis `declared` / `requested`.
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, "+ param") + countIn(body, "param +") + countIn(body, "String(param"),
      "C9 : expected_version ne doit jamais etre journalise brut");
  TEST_ASSERT_EQUAL_INT(1, countIn(body, "param.c_str()"));
  free(body);
  TEST_ASSERT_EQUAL_INT(2, countIn(src, "\"expected_version\""));  // hasParam + getParam, dans cette seule fonction
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, "storeSigSource(g_sigSource[0], param"), "C9");
  free(src);
}

// [C8/C11] aucune mention de l'interrupteur dans l'authentification ni la vérification de signature.
void test_d3b2_aucune_mention_dans_auth_et_ota_signature(void) {
  const char* files[] = {"src/auth.cpp", "src/auth.h", "src/ota_signature.cpp", "src/ota_signature.h",
                         "src/ota_manifest_logic.cpp", "src/ota_sig_relay_logic.cpp", "src/ota_authenticity_logic.cpp"};
  for (const char* f : files) {
    long len = 0;
    char* src = readProjectFile(f, &len);
    char msg[120];
    snprintf(msg, sizeof(msg), "%s introuvable — garde aveugle", f);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, msg);
    snprintf(msg, sizeof(msg), "REGRESSION D3-b2 (C8) : otaSignatureSwitch mentionne dans %s", f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, "otaSignatureSwitch"), msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, "kOtaSignatureSwitch"), msg);
    free(src);
  }
}

// [C2] routes EXACTES : le mode par défaut de server->on(const char*) capte les sous-chemins.
void test_d3b2_routes_de_l_interrupteur_en_matcher_exact(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota_switch.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, countIn(src, "server->on("), "3 routes : arm, etat, desarmement");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, countIn(src, "server->on(AsyncURIMatcher::exact("),
      "C2 : CHAQUE route en AsyncURIMatcher::exact — sinon /ota-signature-switch capte /ota-signature-switch/arm");
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "AsyncURIMatcher::exact(kOtaSignatureSwitchArmRoutePath), HTTP_POST"));
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "AsyncURIMatcher::exact(kOtaSignatureSwitchRoutePath), HTTP_GET"));
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "AsyncURIMatcher::exact(kOtaSignatureSwitchRoutePath), HTTP_POST"));
  // C3 : 403 sur auth coupee / premier demarrage, dans la route d'armement.
  TEST_ASSERT_EQUAL_INT(1, countIn(src, "!authManager.isEnabled() || authManager.isFirstBootDetected()"));
  free(src);
}

// =============================================================================
// [G9] feature-151 D3-c : le REFUS d'une image non authentique.
//   (a) la sortie précoce de /update : markUploadRejected, failOtaUpload, onDisconnect, return,
//       toutes AVANT `g_uploadOwner = request;` ;
//   (b) les 2 return neufs sont en amont des 3 setOtaInProgress(true) ;
//   (c) la séparation TYPÉE : `ui` / pageAnnounced n'atteint pas otaRefusalDecide ;
//   (d) phrases constantes (bornes mesurées dans test_native_ota_refusal_messages) ;
//   (e) INVERSE : empreinte du manifeste illisible => REFUS précoce (plus de repli muet) ;
//   (f) aucun otaManager hors ota_manager.* ;
//   (g) les marqueurs de tolérance D3-a et la mention « aucun refus » ont disparu du code.
// =============================================================================
static bool refusalBlock(const char* src, const char* cond, long* open, long* close) {
  const long at = indexOf(src, cond);
  if (at < 0) return false;
  const char* b = strchr(src + at, '{');
  if (b == NULL) return false;
  *open = (long)(b - src);
  *close = findMatchingBrace(src, *open);
  return *close > *open;
}

void test_g9a_sortie_precoce_update_sequence_complete(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  long bo = 0, bc = 0;
  TEST_ASSERT_TRUE(refusalBlock(src, "if (refusal.rejected && !g_uploadSwitchWaived)", &bo, &bc));
  char* blk = sliceCopy(src, bo, bc);
  const long mark = indexOf(blk, "markUploadRejected(request);");
  const long fail = indexOf(blk, "failOtaUpload(request, refusal.http");
  const long disc = indexOf(blk, "request->onDisconnect(");
  const long ret = indexOf(blk, "return;");
  TEST_ASSERT_TRUE_MESSAGE(mark >= 0, "G9a : markUploadRejected absent -> reponse OK + redemarrage parasite, compteurs NVS perdus");
  TEST_ASSERT_TRUE_MESSAGE(fail >= 0, "G9a : failOtaUpload absent");
  TEST_ASSERT_TRUE_MESSAGE(disc >= 0, "G9a : onDisconnect absent (verdict fantome)");
  TEST_ASSERT_TRUE_MESSAGE(ret >= 0, "G9a : return absent");
  TEST_ASSERT_TRUE_MESSAGE(mark < fail && fail < disc && disc < ret, "G9a : ordre mark < fail < onDisconnect < return");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(blk, "return"), "G9a : un seul return dans la sortie");
  free(blk);
  // La sortie est AVANT la prise de propriete.
  const long owner = indexOf(src, "g_uploadOwner = request;");
  TEST_ASSERT_TRUE(owner > 0);
  TEST_ASSERT_TRUE_MESSAGE(bc < owner, "G9a : la sortie doit preceder g_uploadOwner = request");
  free(src);
}

void test_g9b_returns_en_amont_des_setOtaInProgress_true(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, countIn(src, "setOtaInProgress(true)"), "G9b : 3 sites setOtaInProgress(true)");
  long uo = 0, uc = 0, do_ = 0, dc = 0, bo = 0, bc = 0;
  TEST_ASSERT_TRUE(functionBody(src, "static void handleOtaUpdate(AsyncWebServerRequest* request, const String& filename", &uo, &uc));
  TEST_ASSERT_TRUE(functionBody(src, "static void handleDownloadUpdate(AsyncWebServerRequest* request) {", &do_, &dc));

  // /update : sortie < 1er setOtaInProgress(true) ; /download-update : sortie < 1er aussi.
  TEST_ASSERT_TRUE(refusalBlock(src, "if (refusal.rejected && !g_uploadSwitchWaived)", &bo, &bc));
  const long uTrue = indexOf(src + uo, "setOtaInProgress(true)") + uo;
  TEST_ASSERT_TRUE_MESSAGE(bc < uTrue, "G9b : le return de /update doit preceder setOtaInProgress(true)");
  TEST_ASSERT_TRUE_MESSAGE(uTrue < uc, "G9b : setOtaInProgress(true) de /update dans handleOtaUpdate");

  TEST_ASSERT_TRUE(refusalBlock(src, "if (refusal.rejected && !switchWaived)", &bo, &bc));
  char* blk = sliceCopy(src, bo, bc);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(blk, "return;"), "G9b : le refus de /download-update se termine par return");
  free(blk);
  const long dTrue1 = nthIndexOf(src + do_, "setOtaInProgress(true)", 1) + do_;
  TEST_ASSERT_TRUE_MESSAGE(bo > do_ && bc < dTrue1, "G9b : le return de /download-update precede setOtaInProgress(true)");
  TEST_ASSERT_TRUE(nthIndexOf(src + do_, "setOtaInProgress(true)", 2) >= 0);

  // Appariement : chaque setOtaInProgress(true) a ses setOtaInProgress(false) (comptes mesures
  // par fonction ; supprimer un false laisse la regulation bloquee « OTA en cours »).
  char* up = sliceCopy(src, uo, uc);
  char* dl = sliceCopy(src, do_, dc);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(up, "setOtaInProgress(true)"), "G9b : /update : 1 true");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(dl, "setOtaInProgress(true)"), "G9b : /download-update : 2 true");
  TEST_ASSERT_EQUAL_INT_MESSAGE(6, countIn(up, "setOtaInProgress(false)"), "G9b : /update : 6 false (aucun retire)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(8, countIn(dl, "setOtaInProgress(false)"), "G9b : /download-update : 8 false (aucun retire)");
  free(up);
  free(dl);
  free(src);
}

// (c) La séparation typée : la DÉCISION ne reçoit aucun paramètre de page.
void test_g9c_decision_sans_parametre_de_page(void) {
  long len = 0;
  char* h = readProjectFile("src/ota_refusal_messages.h", &len);
  char* c = readProjectFile("src/ota_refusal_messages.cpp", &len);
  TEST_ASSERT_NOT_NULL(h);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_INT(1, countIn(h, "OtaRefusalDecision otaRefusalDecide(OtaAuthenticityVerdict verdict);"));
  TEST_ASSERT_EQUAL_INT(1, countIn(c, "OtaRefusalDecision otaRefusalDecide(OtaAuthenticityVerdict verdict) {"));
  long o = 0, cl = 0;
  TEST_ASSERT_TRUE(functionBody(c, "OtaRefusalDecision otaRefusalDecide(", &o, &cl));
  char* body = sliceCopy(c, o, cl);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, "pageAnnounced") + countIn(body, "ui"),
                                "G9c : le marqueur ui ne doit JAMAIS atteindre la decision");
  free(body);
  // Sites d'appel : 4 (2 verdicts d'authenticite, 2 refus precoces d'empreinte illisible a verdict
  // CONSTANT). Aucun argument ne peut etre autre chose que l'un de ces deux textes exacts.
  char* r = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, countIn(r, "otaRefusalDecide("), "G9c : 4 sites d'appel de la decision");
  TEST_ASSERT_EQUAL_INT(2, countIn(r, "otaRefusalDecide(authenticity.verdict);"));
  TEST_ASSERT_EQUAL_INT(2, countIn(r, "otaRefusalDecide(OtaAuthenticityVerdict::kMalformed);"));
  // Renfort (revue de code) : `ui` n'est lu qu'a 4 endroits, tous DANS un bloc de refus, jamais
  // stocke (statique, membre) pour atteindre la decision par la bande.
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, countIn(r, "hasParam(\"ui\", true)"), "G9c : `ui` lu 4 fois exactement");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(r, "const bool pageAnnounced = request->hasParam(\"ui\", true);"),
                                "G9c : 2 lectures locales const bool pageAnnounced");
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, countIn(r, "pageAnnounced"), "G9c : pageAnnounced = 2 declarations + 2 usages, rien d'autre");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(r, "static bool pageAnnounced") + countIn(r, "g_pageAnnounced") +
                                       countIn(r, "static const bool pageAnnounced"),
                                "G9c : ui stocke dans une statique");
  long bo = 0, bc = 0;
  // 2 lectures locales : dans les blocs `if (refusal.rejected ...)`.
  TEST_ASSERT_TRUE(refusalBlock(r, "if (refusal.rejected && !g_uploadSwitchWaived)", &bo, &bc));
  char* blk = sliceCopy(r, bo, bc);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(blk, "hasParam(\"ui\", true)"), "G9c : /update : 1 lecture de ui, dans le bloc de refus");
  free(blk);
  TEST_ASSERT_TRUE(refusalBlock(r, "if (refusal.rejected && !switchWaived)", &bo, &bc));
  blk = sliceCopy(r, bo, bc);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(blk, "hasParam(\"ui\", true)"), "G9c : /download-update : 1 lecture de ui, dans le bloc de refus");
  free(blk);
  // 2 lectures en ligne : dans le bloc `else` du decodage d'empreinte, qui reste un refus.
  for (int n = 1; n <= 2; ++n) {
    const long at = nthIndexOf(r, "const OtaRefusalDecision bad = otaRefusalDecide(", n);
    TEST_ASSERT_TRUE(at > 0);
    const long ui = indexOf(r + at, "hasParam(\"ui\", true)");
    TEST_ASSERT_TRUE(ui > 0);
    TEST_ASSERT_TRUE_MESSAGE(ui < 600, "G9c : la lecture de ui doit rester dans le bloc de refus d'empreinte");
    char* seg = sliceCopy(r, at, at + ui);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(seg, "static ") + countIn(seg, ";\n}"), "G9c : lecture de ui hors bloc de refus");
    free(seg);
  }
  free(r);
  free(h);
  free(c);
}

// (d) phrases constantes : le module n'assemble rien ; les sites d'appel ne copient pas dans un tampon.
void test_g9d_phrases_constantes(void) {
  long len = 0;
  char* c = readProjectFile("src/ota_refusal_messages.cpp", &len);
  TEST_ASSERT_NOT_NULL(c);
  const char* forbidden[] = {"String", "snprintf", "sprintf", "malloc", "new ", "strcat", "strcpy", "strncpy"};
  for (const char* f : forbidden) {
    char msg[100];
    snprintf(msg, sizeof(msg), "G9d : « %s » dans ota_refusal_messages.cpp (phrase non constante)", f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(c, f), msg);
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(9, countIn(c, "static const char kPhrase"), "G9d : 9 phrases constantes");
  free(c);
  char* js = readProjectFile("data/app.js", &len);
  TEST_ASSERT_NOT_NULL(js);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(js, "if (cause.length > 200) cause = cause.slice(0, 200);"),
      "G9d : la troncature JS a change : re-mesurer les phrases (borne 200 caracteres)");
  free(js);
  char* r = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(r, "char phrase["), "G9d : aucun tampon char[] local pour les phrases");
  free(r);
}

// (e) INVERSE (revue de securite P2) : si le decodage de l'empreinte du manifeste echoue, la
// signature etant VALIDE, on REFUSE avant toute ecriture. Un repli muet lierait une signature
// valide a une image quelconque.
void test_g9e_empreinte_illisible_refusee(void) {
  long len = 0;
  char* src = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  const char* cond = "if (parseSha256Digest(prefixed, manifestDigest)) {";
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countIn(src, cond), "G9e : 2 sites de decodage d'empreinte du manifeste");
  for (int n = 1; n <= 2; ++n) {
    const long at = nthIndexOf(src, cond, n);
    const long open = at + (long)strlen(cond) - 1;
    const long close = findMatchingBrace(src, open);
    TEST_ASSERT_TRUE(close > open);
    const char* after = src + close + 1;
    while (*after == ' ' || *after == '\n' || *after == '\r' || *after == '\t') ++after;
    TEST_ASSERT_TRUE_MESSAGE(strncmp(after, "else", 4) == 0, "G9e : branche else ABSENTE = repli muet (signature valide + image quelconque)");
    const long eo = (long)(strchr(after, '{') - src);
    const long ec = findMatchingBrace(src, eo);
    TEST_ASSERT_TRUE(ec > eo);
    char* blk = sliceCopy(src, eo, ec);
    TEST_ASSERT_TRUE_MESSAGE(indexOf(blk, "otaRefusalDecide(OtaAuthenticityVerdict::kMalformed)") >= 0, "G9e : verdict kMalformed attendu");
    TEST_ASSERT_TRUE_MESSAGE(indexOf(blk, "OtaRefusalSite::kEarly") >= 0, "G9e : phrase du site precoce");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(blk, "return;"), "G9e : un seul return");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(blk, "setOtaInProgress(true)") + countIn(blk, "g_uploadOwner = "),
                                  "G9e : le refus precede toute prise de propriete / blocage regulation");
    if (n == 1) {  // /update : sequence complete des refus voisins
      const long mark = indexOf(blk, "markUploadRejected(request);");
      const long fail = indexOf(blk, "failOtaUpload(request, bad.http");
      const long disc = indexOf(blk, "request->onDisconnect(");
      const long ret = indexOf(blk, "return;");
      TEST_ASSERT_TRUE_MESSAGE(mark >= 0 && fail >= 0 && disc >= 0 && ret >= 0, "G9e : sequence markUploadRejected/failOtaUpload/onDisconnect/return incomplete");
      TEST_ASSERT_TRUE_MESSAGE(mark < fail && fail < disc && disc < ret, "G9e : ordre mark < fail < onDisconnect < return");
      TEST_ASSERT_TRUE_MESSAGE(indexOf(blk, "true);") > fail, "G9e : failOtaUpload critical");
    } else {  // /download-update : critical + JSON + return
      const long crit = indexOf(blk, "systemLogger.critical(");
      const long send = indexOf(blk, "request->send(bad.http");
      const long ret = indexOf(blk, "return;");
      TEST_ASSERT_TRUE_MESSAGE(crit >= 0 && send >= 0 && ret >= 0, "G9e : critical/send/return incomplet");
      TEST_ASSERT_TRUE_MESSAGE(crit < send && send < ret, "G9e : ordre critical < send < return");
      TEST_ASSERT_TRUE_MESSAGE(indexOf(blk, "badJson[\"code\"] = bad.code;") >= 0, "G9e : code sig_malformee absent du JSON");
    }
    free(blk);
  }
  free(src);
}

// (f) aucun otaManager hors ota_manager.* : on relit les sources de ce lot.
void test_g9f_aucun_otaManager_hors_ota_manager(void) {
  const char* files[] = {"src/web_routes_ota.cpp", "src/web_routes_ota_switch.cpp", "src/ota_signature_switch.cpp",
                         "src/ota_refusal_messages.cpp", "src/main.cpp", "src/web_server.cpp", "src/mqtt_manager.cpp"};
  for (const char* f : files) {
    long len = 0;
    char* src = readProjectFile(f, &len);
    char msg[120];
    snprintf(msg, sizeof(msg), "%s introuvable — garde aveugle", f);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, msg);
    snprintf(msg, sizeof(msg), "G9f : %s reference otaManager / ArduinoOTA (contournement du refus D3-c)", f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, "otaManager") + countIn(src, "ota_manager.h") + countIn(src, "ArduinoOTA"), msg);
    free(src);
  }
}

// Lecture BRUTE (commentaires conservés) : (g) vise justement les commentaires périmés.
static char* readRawSrc(const char* rel) {
  char path[600];
  const char* f = __FILE__;
  const char* m = strstr(f, "test/test_native_ota_order_guard/");
  if (m == NULL) return NULL;
  const size_t n = (size_t)(m - f);
  memcpy(path, f, n);
  path[n] = '\0';
  strncat(path, rel, sizeof(path) - strlen(path) - 1);
  FILE* fp = fopen(path, "rb");
  if (fp == NULL) return NULL;
  fseek(fp, 0, SEEK_END);
  const long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char* buf = (char*)malloc((size_t)len + 1);
  const size_t got = fread(buf, 1, (size_t)len, fp);
  fclose(fp);
  buf[got] = '\0';
  return buf;
}

// kBelowFloor reste inatteignable : plancher fige a 0.0.0 (decision utilisateur : /update reste
// installable avec une version anterieure signee). Et le desaccord de version reste journal seul.
void test_g9h_plancher_fige_et_version_journal_seul(void) {
  long len = 0;
  char* k = readProjectFile("src/constants.h", &len);
  TEST_ASSERT_NOT_NULL(k);
  TEST_ASSERT_EQUAL_INT(1, countIn(k, "kOtaVersionFloorDefaultMajor = 0;"));
  TEST_ASSERT_EQUAL_INT(1, countIn(k, "kOtaVersionFloorDefaultMinor = 0;"));
  TEST_ASSERT_EQUAL_INT(1, countIn(k, "kOtaVersionFloorDefaultPatch = 0;"));
  free(k);
  char* r = readProjectFile("src/web_routes_ota.cpp", &len);
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(r, "otaRefusalDecide(authenticity.verdict)") - 2, "decision : 2 sites");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(r, "kBelowFloor"), "G9h : aucun traitement special de kBelowFloor dans les routes");
  free(r);
}

// (g) Les mentions « aucun refus appliqué » et « TOLÉRANCE D3-a » ont disparu du code.
// Les needles sont assemblés pour que ce fichier ne se contienne pas lui-même.
void test_g9g_marqueurs_perimes_disparus(void) {
  const char* files[] = {"src/web_routes_ota.cpp", "src/web_routes_ota_switch.cpp", "src/ota_signature_switch.cpp",
                         "src/ota_signature_switch.h", "src/ota_authenticity_logic.h", "src/ota_refusal_messages.cpp",
                         "src/ota_refusal_messages.h", "data/app.js", "data/index.html", "src/constants.h",
                         "src/ota_recovery_page.cpp", "src/ota_recovery_page.html.in"};
  // Aiguilles SPECIFIQUES (pas de mot nu : « INERTE » designe aussi la temperature figee dans
  // constants.h, « journalise seulement » est legitime dans ota_signature_switch.cpp).
  const char* extra[] = {"gardes" " inertes", "lot" " inerte", "jamais relue pour" " accorder",
                         "aucun refus pour" " l'instant", "pas encore" " activé"};
  const char* needleA = "aucun refus" " appliqué";
  const char* needleB = "TOLÉRANCE" " D3-a";
  for (const char* f : files) {
    char* src = readRawSrc(f);
    char msg[140];
    snprintf(msg, sizeof(msg), "%s introuvable — garde aveugle", f);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, msg);
    // Comparaison insensible à la casse pour A (grep -i).
    for (char* p = src; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    snprintf(msg, sizeof(msg), "G9g : mention perimee « aucun refus applique » dans %s", f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, needleA), msg);
    free(src);
    src = readRawSrc(f);
    snprintf(msg, sizeof(msg), "G9g : marqueur « TOLERANCE D3-a » dans %s", f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, needleB), msg);
    free(src);
    src = readRawSrc(f);
    for (char* p = src; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    for (const char* x : extra) {
      snprintf(msg, sizeof(msg), "G9g : mention perimee « %s » dans %s", x, f);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(src, x), msg);
    }
    free(src);
  }
}

// (C) kPhraseFallback existe en 4 exemplaires (module C++, app.js, 2 pages de secours) : leur
// identite est figee, sinon la page de secours et la carte diraient deux phrases differentes.
static bool quotedAfter(const char* src, const char* marker, char* out, size_t cap) {
  const long at = indexOf(src, marker);
  if (at < 0) return false;
  const char* q = strchr(src + at + strlen(marker), '"');
  if (q == NULL) return false;
  const char* e = strchr(q + 1, '"');
  if (e == NULL || (size_t)(e - q - 1) >= cap) return false;
  memcpy(out, q + 1, (size_t)(e - q - 1));
  out[e - q - 1] = '\0';
  return true;
}

void test_g9i_phrase_de_repli_identique_partout(void) {
  struct Src { const char* file; const char* marker; };
  const Src srcs[] = {{"src/ota_refusal_messages.cpp", "kPhraseFallback[] ="},
                      {"data/app.js", "OTA_REFUSAL_FALLBACK ="},
                      {"src/ota_recovery_page.cpp", "fail(c ||"},
                      {"src/ota_recovery_page.html.in", "fail(c ||"}};
  char ref[300] = {0};
  for (int i = 0; i < 4; ++i) {
    long len = 0;
    char* src = readProjectFile(srcs[i].file, &len);
    TEST_ASSERT_NOT_NULL(src);
    char cur[300] = {0};
    char msg[160];
    snprintf(msg, sizeof(msg), "G9i : phrase de repli introuvable dans %s", srcs[i].file);
    TEST_ASSERT_TRUE_MESSAGE(quotedAfter(src, srcs[i].marker, cur, sizeof(cur)), msg);
    free(src);
    if (i == 0) {
      TEST_ASSERT_TRUE(strlen(cur) > 10);
      strcpy(ref, cur);
    } else {
      snprintf(msg, sizeof(msg), "G9i : phrase de repli de %s differe de celle du module C++", srcs[i].file);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(ref, cur, msg);
    }
  }
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
  RUN_TEST(test_kind_resolution_precedes_magic_and_partition_effects);
  RUN_TEST(test_download_kind_resolution_precedes_partition_choice);
  RUN_TEST(test_download_bodies_carry_manifest_and_signature);
  RUN_TEST(test_d3b2_lectures_et_consommations_comptees_et_ordonnees);
  RUN_TEST(test_d3b2_la_decision_n_est_jamais_une_condition);
  RUN_TEST(test_d3b2_verdict_et_levee_distincts);
  RUN_TEST(test_d3b2_aucune_mention_dans_auth_et_ota_signature);
  RUN_TEST(test_d3b2_routes_de_l_interrupteur_en_matcher_exact);
  RUN_TEST(test_d3b2_liaison_version_est_purement_journalistique);
  RUN_TEST(test_g9a_sortie_precoce_update_sequence_complete);
  RUN_TEST(test_g9b_returns_en_amont_des_setOtaInProgress_true);
  RUN_TEST(test_g9c_decision_sans_parametre_de_page);
  RUN_TEST(test_g9d_phrases_constantes);
  RUN_TEST(test_g9e_empreinte_illisible_refusee);
  RUN_TEST(test_g9f_aucun_otaManager_hors_ota_manager);
  RUN_TEST(test_g9g_marqueurs_perimes_disparus);
  RUN_TEST(test_g9i_phrase_de_repli_identique_partout);
  RUN_TEST(test_g9h_plancher_fige_et_version_journal_seul);
  return UNITY_END();
}
