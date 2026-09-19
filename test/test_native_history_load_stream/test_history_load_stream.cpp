// =============================================================================
// Tests unitaires natifs — F2, lecture en flux de /history.json (feature-131)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32. `HistoryManager::loadFromFile()`
// n'est PAS compilable en natif (fs::File, ArduinoJson::deserializeJson sur un
// flux, FreeRTOS) — même limite structurelle que
// test/test_native_history/test_history_logic.cpp (section « ASSERTIONS SUR
// LES SOURCES ») et test/test_native_history_atomicity/ (section
// « D3-structure »). Cette suite verrouille par le TEXTE les invariants NEUFS
// de la lecture en flux introduits par feature-131 : le contrat clés absentes
// → NaN et le discriminant bool/int/absent de "d" existaient déjà AVANT ce
// lot (bug-098, feature-095) et restent couverts par
// test/test_native_history/test_history_logic.cpp — pas dupliqués ici.
//
// CE QUE CETTE SUITE NE PROUVE PAS : que le parsing octet par octet fonctionne
// réellement sur un vrai flux LittleFS — cela suppose ArduinoJson (non
// dépendance de l'env `native`, ADR-0017) et reste du ressort d'un test
// on-target (plan de test manuel de la spec, point 3).
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "constants.h"

void setUp(void) {}
void tearDown(void) {}

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  const char* prefixes[4] = {"", "../", "../../", "../../../"};
  for (int i = 0; i < 4; ++i) {
    snprintf(path, sizeof(path), "%s%s", prefixes[i], rel);
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
    if (outLen) *outLen = (long)got;
    return buf;
  }
  return NULL;
}

static const char* slice(const char* src, const char* from, const char* to, size_t* outLen) {
  const char* a = strstr(src, from);
  if (a == NULL) return NULL;
  const char* b = strstr(a + strlen(from), to);
  *outLen = (b != NULL) ? (size_t)(b - a) : strlen(a);
  return a;
}

static int countIn(const char* hay, size_t len, const char* needle) {
  int c = 0;
  const size_t n = strlen(needle);
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(hay + i, needle, n) == 0) c++;
  }
  return c;
}
static int containsIn(const char* hay, size_t len, const char* needle) {
  return countIn(hay, len, needle) > 0;
}

// Charge le corps de loadFromFile() une fois pour toute la suite.
static char* gSrc = nullptr;
static const char* gBody = nullptr;
static size_t gBodyLen = 0;

static void loadLoadFromFileBody(void) {
  if (gSrc != nullptr) return;
  long len = 0;
  gSrc = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(gSrc, "src/history.cpp introuvable");
  gBody = slice(gSrc, "void HistoryManager::loadFromFile()",
                "void HistoryManager::migrateLegacyHistory", &gBodyLen);
  TEST_ASSERT_NOT_NULL(gBody);
}

void test_load_prefixe_verifie_octet_par_octet_contre_kHistoryPersistedPrefix(void) {
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(gBody, gBodyLen, "kHistoryPersistedPrefix[i]"),
      "le prefixe du fichier doit etre verifie contre la MEME constante que "
      "l'ecriture (kHistoryPersistedPrefix) — sinon lecture et ecriture peuvent diverger");
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "prefixOk = false;"));
  // En-tête invalide : WARNING (jamais ERROR), fichier fermé, retour immédiat
  // — pas de remove() : un en-tête étranger n'est pas forcément un fichier à
  // détruire (ex. fichier créé par un outil externe).
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "Historique illisible"));
}

void test_load_eof_avant_crochet_fermant_marque_tronque(void) {
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(gBody, gBodyLen, "if (next < 0) { truncated = true; break; }"),
      "l'EOF avant le ']' attendu doit marquer troncature, pas planter ni boucler");
}

void test_load_plafond_dur_present_et_verifie_avant_deserializeJson(void) {
  // Revue code-reviewer (feature-131) : sans ce plafond, un /history.json
  // corrompu ou porteur de doublons RAW ferait croitre memoryBuffer SANS
  // LIMITE au boot, avec le doublement geometrique de capacite que ce lot
  // existe precisement pour eviter. kHistoryLoadMaxPoints doit borner les
  // objets PARSES, pas seulement ceux conserves — d'ou l'exigence de
  // POSITION (avant deserializeJson), pas seulement de PRESENCE.
  loadLoadFromFileBody();
  const char* cap = "if (parsedCount >= kHistoryLoadMaxPoints) {";
  const char* parse = "if (deserializeJson(doc, f)) {";
  TEST_ASSERT_TRUE_MESSAGE(containsIn(gBody, gBodyLen, cap),
      "plafond dur kHistoryLoadMaxPoints absent de loadFromFile — croissance non bornee au boot");
  TEST_ASSERT_TRUE_MESSAGE(containsIn(gBody, gBodyLen, parse),
      "deserializeJson introuvable — le corps de reference a change, revoir ce test");
  const char* posCap = strstr(gBody, cap);
  const char* posParse = strstr(gBody, parse);
  TEST_ASSERT_TRUE_MESSAGE(posCap != NULL && posParse != NULL && posCap < posParse,
      "le plafond doit etre verifie AVANT deserializeJson — sinon il ne borne que "
      "les points CONSERVES, pas ceux PARSES (un JSON hostile continuerait a etre "
      "desserialise sans fin avant d'etre compte)");
}

void test_load_erreur_deserialisation_marque_tronque(void) {
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(gBody, gBodyLen, "if (deserializeJson(doc, f)) {"),
      "chaque objet doit etre verifie individuellement (flux de documents ArduinoJson 7)");
  // Le point ne doit PAS être poussé s'il a échoué : `truncated = true; break;`
  // doit précéder tout push_back pour cette itération — vérifié par comptage :
  // un seul push_back par itération réussie, jamais un push_back après échec
  // de deserializeJson dans le même passage de boucle (structure du code).
  TEST_ASSERT_EQUAL_INT(1, countIn(gBody, gBodyLen, "memoryBuffer.push_back(dp);"));
}

void test_load_separateur_inattendu_marque_tronque(void) {
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(gBody, gBodyLen, "truncated = true;  // séparateur inattendu"),
      "un separateur ni virgule ni crochet fermant doit marquer troncature");
}

void test_load_accolade_fermante_ne_perd_pas_les_points_deja_charges(void) {
  loadLoadFromFileBody();
  // La vérification de l'accolade finale doit suivre la boucle de parsing
  // (donc APRÈS le dernier push_back), et ne JAMAIS vider memoryBuffer —
  // c'est le cœur du « meilleur effort » (D4) : un document tronqué en toute
  // fin garde tous les points déjà complets.
  const char* braceCheck = "if (!truncated && f.read() != '}') truncated = true;";
  TEST_ASSERT_TRUE_MESSAGE(containsIn(gBody, gBodyLen, braceCheck),
      "verification de l'accolade fermante absente ou modifiee");
  const size_t iBrace = (size_t)(strstr(gBody, braceCheck) - gBody);
  const size_t iPush = (size_t)(strstr(gBody, "memoryBuffer.push_back(dp);") - gBody);
  TEST_ASSERT_TRUE_MESSAGE(iBrace > iPush,
      "la verification de l'accolade doit venir APRES la boucle de parsing, "
      "jamais avant (sinon elle interromprait le chargement des points valides)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countIn(gBody, gBodyLen, "memoryBuffer.clear();"),
      "memoryBuffer ne doit etre vide qu'UNE fois, en tete de fonction — jamais "
      "en reaction a une troncature (meilleur effort D4)");
}

void test_load_troncature_journalisee_en_warning_jamais_error(void) {
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE_MESSAGE(containsIn(gBody, gBodyLen, "systemLogger.warning(\"Historique tronqué"),
      "message de troncature absent ou change de niveau");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, containsIn(gBody, gBodyLen, "systemLogger.error("),
      "loadFromFile ne doit jamais logger en ERROR (flush synchrone -> fopen -> abort)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, containsIn(gBody, gBodyLen, "systemLogger.critical("),
      "loadFromFile ne doit jamais logger en CRITICAL");
  // Le message rapporte le compte de points RECUPERES, pas juste "erreur" —
  // observabilite du cout reel d'une troncature.
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "point(s) récupéré(s)"));
}

void test_load_reservation_unique_avant_la_boucle_d2(void) {
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(gBody, gBodyLen, "memoryBuffer.reserve(kHistoryLoadReservePoints);"),
      "reservation unique absente — l'escalade de capacite du vecteur redemande "
      "des blocs contigus croissants (D2)");
  const size_t iReserve = (size_t)(strstr(gBody, "memoryBuffer.reserve(kHistoryLoadReservePoints);") - gBody);
  const size_t iPush = (size_t)(strstr(gBody, "memoryBuffer.push_back(dp);") - gBody);
  TEST_ASSERT_TRUE_MESSAGE(iReserve < iPush,
      "reserve() doit preceder la boucle de chargement, pas la suivre");
}

void test_load_watchdog_reinitialise_tous_les_64_points(void) {
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(gBody, gBodyLen, "(++parsedCount % 64) == 0) esp_task_wdt_reset();"),
      "watchdog non reinitialise periodiquement — boucle de chargement potentiellement longue");
}

void test_load_clefs_absentes_retombent_sur_nan_pas_zero(void) {
  // Invariant PRE-EXISTANT (feature-095), toujours vrai apres le passage en
  // flux — verifie ici parce que c'est la meme fonction, pas pour le
  // dupliquer avec test_native_history/test_history_logic.cpp qui le couvre
  // deja explicitement.
  loadLoadFromFileBody();
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "point[\"p\"] | NAN"));
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "point[\"o\"] | NAN"));
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "point[\"T\"] | NAN"));
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "point[\"pf\"] | NAN"));
  TEST_ASSERT_TRUE(containsIn(gBody, gBodyLen, "point[\"of\"] | NAN"));
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_load_prefixe_verifie_octet_par_octet_contre_kHistoryPersistedPrefix);
  RUN_TEST(test_load_eof_avant_crochet_fermant_marque_tronque);
  RUN_TEST(test_load_plafond_dur_present_et_verifie_avant_deserializeJson);
  RUN_TEST(test_load_erreur_deserialisation_marque_tronque);
  RUN_TEST(test_load_separateur_inattendu_marque_tronque);
  RUN_TEST(test_load_accolade_fermante_ne_perd_pas_les_points_deja_charges);
  RUN_TEST(test_load_troncature_journalisee_en_warning_jamais_error);
  RUN_TEST(test_load_reservation_unique_avant_la_boucle_d2);
  RUN_TEST(test_load_watchdog_reinitialise_tous_les_64_points);
  RUN_TEST(test_load_clefs_absentes_retombent_sur_nan_pas_zero);

  return UNITY_END();
}
