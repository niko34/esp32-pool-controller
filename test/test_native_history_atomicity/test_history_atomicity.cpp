// =============================================================================
// Tests unitaires natifs — atomicité .tmp+rename de /history.json (feature-131)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino /
// LittleFS. `HistoryManager::saveToFile()`/`loadFromFile()`/`reconcileHistoryTmp()`
// ne sont PAS compilables en natif (fs::File, FreeRTOS, ArduinoJson::Stream sur
// fs::File) — même limite déjà posée par
// test/test_native_history/test_history_logic.cpp (section « ASSERTIONS SUR
// LES SOURCES »). Cette suite combine donc DEUX preuves complémentaires :
//
//   D3-comportement : un harnais qui REPRODUIT FIDÈLEMENT (mêmes constantes,
//   même découpage en tampon, même sémantique « sticky ») le puits d'écriture
//   HistoryFileWriteSink/historyFileSink de src/history.cpp, branché sur un
//   FakeFS en mémoire (std::string) au lieu de fs::File — pour prouver
//   BÉHAVIORALEMENT qu'une écriture interrompue ne corrompt jamais l'ancien
//   fichier et que le puits cesse d'accepter des octets dès le premier échec.
//
//   D3-structure : des assertions sur le TEXTE de src/history.cpp et
//   src/history.h qui verrouillent que le harnais ci-dessus correspond
//   TOUJOURS au code réel (mêmes conditions, mêmes appels) — sans elles, le
//   harnais pourrait dériver du code qu'il prétend représenter sans qu'aucun
//   test ne le remarque.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include "constants.h"
#include "history_persist_format.h"

void setUp(void) {}
void tearDown(void) {}

// Réplique LOCALE du contrat HistoryByteSink (src/history.h) — le vrai header
// n'est pas inclus ici (il tire Arduino.h/LittleFS.h/FreeRTOS, indisponibles
// en natif), mais la signature est un simple pointeur de fonction C, recopiée
// à l'identique.
typedef size_t (*LocalHistoryByteSink)(void* ctx, const char* p, size_t n);

// =============================================================================
// D3-comportement — FakeFS + puits d'écriture répliqué
// =============================================================================

// FakeFS minimal : un espace de noms de fichiers en mémoire (std::string),
// remove()/rename() avec la même sémantique que fs::LittleFSFS (rename
// échoue si la source n'existe pas ; remove() renvoie faux si absent).
struct FakeFs {
  bool exists(const std::string& path) const { return files.count(path) != 0; }
  bool remove(const std::string& path) {
    auto it = files.find(path);
    if (it == files.end()) return false;
    files.erase(it);
    return true;
  }
  bool rename(const std::string& from, const std::string& to) {
    auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = it->second;
    files.erase(it);
    return true;
  }
  std::map<std::string, std::string> files;
};

// Réplique de HistoryFileWriteSink (history.cpp) : MÊME buffer, MÊME
// sémantique sticky. `write` remplace `File::write()` — injecte les échecs.
struct MockWriteSink {
  std::string* dest;                  // "fichier" cible dans le FakeFs
  size_t failAfterBytes;              // écrit normalement jusqu'à ce total, puis échec
  size_t totalWrittenToDevice = 0;
  char buf[kHistoryWriteChunkBytes];
  size_t used = 0;
  bool ok = true;

  // Modélise fs::File::write() : renvoie moins que demandé une fois le
  // budget `failAfterBytes` dépassé (comme un LittleFS à court d'espace).
  size_t deviceWrite(const char* p, size_t n) {
    size_t allowed = (totalWrittenToDevice >= failAfterBytes)
                         ? 0
                         : (failAfterBytes - totalWrittenToDevice);
    const size_t w = (n < allowed) ? n : allowed;
    dest->append(p, w);
    totalWrittenToDevice += w;
    return w;
  }

  bool flush() {
    if (!ok) return false;
    if (used == 0) return true;
    const size_t expected = used;
    const size_t w = deviceWrite(buf, expected);
    used = 0;
    if (w != expected) ok = false;  // MÊME comparaison stricte que history.cpp
    return ok;
  }
};

// Réplique de historyFileSink (history.cpp) : découpage par
// kHistoryWriteChunkBytes, arrêt sticky sur échec.
static size_t mockFileSink(void* ctx, const char* p, size_t n) {
  MockWriteSink* sink = static_cast<MockWriteSink*>(ctx);
  if (!sink->ok) return 0;
  size_t total = 0;
  while (n > 0) {
    const size_t space = sizeof(sink->buf) - sink->used;
    const size_t chunk = (n < space) ? n : space;
    memcpy(sink->buf + sink->used, p, chunk);
    sink->used += chunk;
    p += chunk;
    n -= chunk;
    total += chunk;
    if (sink->used == sizeof(sink->buf) && !sink->flush()) break;
  }
  return total;
}

// Réplique de HistoryManager::emitPersistedJson() (history.cpp) sur un jeu de
// points RAW fixe — mêmes fonctions pures que test_native_history_persist_format
// (kHistoryPersistedPrefix/Suffix, formatPersistedHistoryPoint).
static size_t emitOverSink(LocalHistoryByteSink sink, void* ctx, size_t pointCount) {
  size_t total = 0;
  total += sink(ctx, kHistoryPersistedPrefix, sizeof(kHistoryPersistedPrefix) - 1);
  bool first = true;
  for (size_t i = 0; i < pointCount; ++i) {
    char buf[kHistoryPersistPointMaxBytes];
    const size_t n = formatPersistedHistoryPoint(buf, sizeof(buf), first,
                                                 1757000000UL + i * 300, 7.2f, 650.0f,
                                                 24.0f, 7.2f, 650.0f, true, 0, 0);
    if (n > 0) {
      total += sink(ctx, buf, n);
      first = false;
    }
  }
  total += sink(ctx, kHistoryPersistedSuffix, sizeof(kHistoryPersistedSuffix) - 1);
  return total;
}

// Simule saveToFile() de bout en bout sur le FakeFs : passe 1 (mesure), passe
// 2 (écriture, avec échec injecté via failAfterBytes), puis la même décision
// que history.cpp (rename si written==jsonSize && flushOk, sinon suppression
// du .tmp SEUL — jamais de l'ancien).
struct SaveOutcome {
  bool committed;    // true si le rename a eu lieu
  bool flushOk;       // vidage final du tampon d'accumulation réussi
  size_t written;
  size_t jsonSize;
};

static SaveOutcome simulateSaveToFile(FakeFs& fs, const std::string& oldContent,
                                      size_t pointCount, size_t failAfterBytes) {
  const std::string finalPath = "/history.json";
  const std::string tmpPath = "/history.json.tmp";
  fs.files[finalPath] = oldContent;  // ancien fichier déjà en place

  size_t jsonSize = 0;
  auto counterSink = [](void* ctx, const char* p, size_t n) -> size_t {
    (void)p;
    *static_cast<size_t*>(ctx) += n;
    return n;
  };
  jsonSize = 0;
  {
    size_t measured = 0;
    emitOverSink(+[](void* ctx, const char* p, size_t n) -> size_t {
      (void)p;
      *static_cast<size_t*>(ctx) += n;
      return n;
    }, &measured, pointCount);
    jsonSize = measured;
  }

  fs.files[tmpPath] = "";  // open("w") : le .tmp est créé vide
  MockWriteSink sink{&fs.files[tmpPath], failAfterBytes};
  const size_t written = emitOverSink(mockFileSink, &sink, pointCount);
  const bool flushOk = sink.flush();

  SaveOutcome outcome{false, flushOk, written, jsonSize};
  if (!flushOk || written != jsonSize) {
    // MÊME décision que history.cpp : le .tmp est supprimé, JAMAIS l'ancien.
    fs.remove(tmpPath);
    return outcome;
  }
  outcome.committed = fs.rename(tmpPath, finalPath);
  return outcome;
}

void test_atomicite_ecriture_complete_admise_rename_remplace_ancien(void) {
  FakeFs fs;
  const SaveOutcome outcome = simulateSaveToFile(fs, "ANCIEN_CONTENU", 5, SIZE_MAX);
  TEST_ASSERT_TRUE(outcome.committed);
  TEST_ASSERT_EQUAL_size_t(outcome.jsonSize, outcome.written);
  TEST_ASSERT_FALSE_MESSAGE(fs.exists("/history.json.tmp"),
      "le .tmp doit disparaitre apres un rename reussi");
  TEST_ASSERT_TRUE(fs.exists("/history.json"));
  TEST_ASSERT_TRUE_MESSAGE(fs.files["/history.json"] != "ANCIEN_CONTENU",
      "le rename doit avoir remplace le contenu par le nouveau document");
}

void test_atomicite_echec_ecriture_tmp_supprime_ancien_intact(void) {
  // Échec injecté à mi-parcours (avant la fin du document, mais AVANT que le
  // tampon d'accumulation n'atteigne kHistoryWriteChunkBytes) : le document
  // entier (5 points, < 512 o) tient dans un seul tampon, donc les octets
  // sont tous « acceptés » (bufferisés) par le puits — c'est le flush() FINAL,
  // hors boucle, qui échoue. C'est exactement pourquoi saveToFile() teste
  // `!flushOk || written != jsonSize` et pas written seul (cf. history.cpp).
  FakeFs fs;
  const std::string ancien = "ANCIEN_CONTENU_INTACT";
  const SaveOutcome outcome = simulateSaveToFile(fs, ancien, 5, /*failAfterBytes=*/40);
  TEST_ASSERT_FALSE_MESSAGE(outcome.committed, "un rename n'a jamais du avoir lieu");
  TEST_ASSERT_FALSE_MESSAGE(outcome.flushOk,
      "le vidage final du tampon doit avoir echoue (deviceWrite tronque a 40 o)");
  TEST_ASSERT_FALSE_MESSAGE(fs.exists("/history.json.tmp"),
      "le .tmp rate doit etre supprime, jamais laisse en place");
  TEST_ASSERT_TRUE_MESSAGE(fs.exists("/history.json"),
      "GAIN NET de l'atomicite : l'ancien /history.json doit survivre a l'echec");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(ancien.c_str(), fs.files["/history.json"].c_str(),
      "l'ancien contenu ne doit PAS avoir ete altere par l'echec du nouveau");
}

void test_atomicite_puits_reste_sticky_apres_le_premier_echec(void) {
  // Mutant F3 n°4 : si un puits « buggé » renvoyait `len` au lieu du nombre
  // reel d'octets ecrits, ce test ne verrait plus jamais written < jsonSize.
  FakeFs fs;
  fs.files["/history.json.tmp"] = "";
  MockWriteSink sink{&fs.files["/history.json.tmp"], /*failAfterBytes=*/10};
  const size_t written = emitOverSink(mockFileSink, &sink, 5);
  const bool flushOk = sink.flush();
  TEST_ASSERT_FALSE(flushOk);
  TEST_ASSERT_FALSE_MESSAGE(sink.ok, "le puits doit rester sticky (ok=false) apres l'echec");
  // Un appel supplémentaire APRÈS l'échec ne doit plus rien accepter.
  const size_t extra = mockFileSink(&sink, "X", 1);
  TEST_ASSERT_EQUAL_size_t(0, extra);
  (void)written;
}

void test_atomicite_echec_exactement_a_la_derniere_octet_du_tampon(void) {
  // Frontière : l'échec survient PILE au vidage du tampon de
  // kHistoryWriteChunkBytes — vérifie que la détection n'a pas d'angle mort à
  // la limite du tampon.
  FakeFs fs;
  const std::string ancien = "X";
  const SaveOutcome outcome = simulateSaveToFile(fs, ancien, 20, kHistoryWriteChunkBytes);
  TEST_ASSERT_FALSE(outcome.committed);
  TEST_ASSERT_TRUE(fs.exists("/history.json"));
  TEST_ASSERT_EQUAL_STRING(ancien.c_str(), fs.files["/history.json"].c_str());
}

void test_atomicite_echec_en_cours_de_flux_written_inferieur_a_jsonSize(void) {
  // Document assez gros (20 points) pour dépasser plusieurs fois
  // kHistoryWriteChunkBytes : l'échec survient DANS la boucle (flush()
  // intermédiaire appelé par mockFileSink), pas seulement au flush final.
  // Ici written < jsonSize est directement observable — l'AUTRE moitié de la
  // condition `!flushOk || written != jsonSize` de saveToFile().
  FakeFs fs;
  const std::string ancien = "ANCIEN";
  const SaveOutcome outcome = simulateSaveToFile(fs, ancien, 20, /*failAfterBytes=*/700);
  TEST_ASSERT_FALSE(outcome.committed);
  TEST_ASSERT_TRUE_MESSAGE(outcome.written < outcome.jsonSize,
      "un echec en cours de flux doit se voir sur written, pas seulement sur flushOk");
  TEST_ASSERT_EQUAL_STRING(ancien.c_str(), fs.files["/history.json"].c_str());
}

// -----------------------------------------------------------------------
// reconcileHistoryTmp() — réplique du contrat (remove() SEUL, inconditionnel)
// -----------------------------------------------------------------------
static bool mockReconcileHistoryTmp(FakeFs& fs) {
  return fs.remove("/history.json.tmp");  // MÊME contrat : remove() seul, sans exists()
}

void test_reconcile_orphelin_efface_ancien_intact(void) {
  FakeFs fs;
  fs.files["/history.json"] = "ANCIEN";
  fs.files["/history.json.tmp"] = "DECHET_ORPHELIN";
  const bool hadOrphan = mockReconcileHistoryTmp(fs);
  TEST_ASSERT_TRUE(hadOrphan);
  TEST_ASSERT_FALSE(fs.exists("/history.json.tmp"));
  TEST_ASSERT_TRUE(fs.exists("/history.json"));
  TEST_ASSERT_EQUAL_STRING("ANCIEN", fs.files["/history.json"].c_str());
}

void test_reconcile_sans_orphelin_est_un_no_op(void) {
  FakeFs fs;
  fs.files["/history.json"] = "ANCIEN";
  const bool hadOrphan = mockReconcileHistoryTmp(fs);
  TEST_ASSERT_FALSE(hadOrphan);
  TEST_ASSERT_TRUE(fs.exists("/history.json"));
}

// =============================================================================
// D3-structure — verrouille que le harnais ci-dessus reflète le VRAI code
// =============================================================================

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

static int containsIn(const char* hay, size_t len, const char* needle) {
  const size_t n = strlen(needle);
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(hay + i, needle, n) == 0) return 1;
  }
  return 0;
}

void test_source_flush_compare_stricte_w_expected(void) {
  // Mutant F3 n°4 (puits qui renverrait `len` au lieu du nombre reel ecrit) :
  // ce test verrouille que history.cpp COMPARE bien `w` (retour de
  // File::write) a `expected`, pas une constante ou `n`.
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/history.cpp introuvable");
  size_t bodyLen = 0;
  const char* body = slice(src, "struct HistoryFileWriteSink", "size_t historyFileSink", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, "if (w != expected) ok = false;"),
      "detection d'ecriture courte absente ou affaiblie dans HistoryFileWriteSink::flush()");
  free(src);
}

void test_source_saveToFile_supprime_le_tmp_jamais_l_ancien_sur_echec(void) {
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::saveToFile()",
                           "void HistoryManager::loadFromFile()", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(body, bodyLen, "if (!flushOk || written != jsonSize) {"),
      "condition de detection d'ecriture partielle absente ou modifiee");
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(body, bodyLen, "historyStore->remove(historyTmpFilePath);"),
      "l'echec doit supprimer le .tmp (historyTmpFilePath), jamais l'ancien fichier");
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(body, bodyLen, "historyStore->rename(historyTmpFilePath, historyFilePath)"),
      "la bascule finale doit passer par rename(), seule operation qui remplace l'ancien");
  free(src);  // `body` pointe DANS `src` : liberer seulement apres le dernier usage
}

void test_source_reconcile_appelle_remove_seul_sans_open_ni_exists(void) {
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::reconcileHistoryTmp()",
                           "size_t HistoryManager::emitPersistedJson", &bodyLen);
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "reconcileHistoryTmp() introuvable dans history.cpp");
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, "historyStore->remove(historyTmpFilePath)"));
  TEST_ASSERT_FALSE_MESSAGE(containsIn(body, bodyLen, "->open("),
      "reconcileHistoryTmp ne doit JAMAIS ouvrir de fichier (0 site fopen, ADR-0070)");
  TEST_ASSERT_FALSE_MESSAGE(containsIn(body, bodyLen, "->exists("),
      "exists() est une ouverture deguisee (VFSFileImpl) — interdite ici aussi");
  free(src);  // `body` pointe DANS `src` : liberer seulement apres le dernier usage
}

void test_source_reconcile_appele_dans_begin_avant_loadFromFile(void) {
  // Mutant F3 n°5 : supprimer l'appel a reconcileHistoryTmp() dans begin().
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::begin()",
                           "void HistoryManager::update()", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  const char* reconcileCall = "reconcileHistoryTmp();";
  const char* loadCall = "loadFromFile();";
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, reconcileCall),
      "reconcileHistoryTmp() n'est plus appelee depuis begin() — .tmp orphelins jamais nettoyes");
  TEST_ASSERT_TRUE(containsIn(body, bodyLen, loadCall));
  const char* posReconcile = strstr(body, reconcileCall);
  const char* posLoad = strstr(body, loadCall);
  TEST_ASSERT_TRUE_MESSAGE(posReconcile != NULL && posLoad != NULL && posReconcile < posLoad,
      "reconcileHistoryTmp() doit s'executer AVANT loadFromFile()");
  free(src);
}

void test_source_avertissements_jamais_error_sur_le_chemin_atomicite(void) {
  // D4/A6 : un ERROR flusherait le journal en synchrone -> fopen() ->
  // exactement l'abort() que ce lot evite. Verrouille WARNING partout sur le
  // chemin d'echec d'ecriture/rename/reconciliation.
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::saveToFile()",
                           "void HistoryManager::loadFromFile()", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, containsIn(body, bodyLen, "systemLogger.error("),
      "saveToFile ne doit jamais logger en ERROR (flush synchrone -> fopen -> abort)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, containsIn(body, bodyLen, "systemLogger.critical("),
      "saveToFile ne doit jamais logger en CRITICAL");
  free(src);  // `body` pointe DANS `src` : liberer seulement apres le dernier usage
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_atomicite_ecriture_complete_admise_rename_remplace_ancien);
  RUN_TEST(test_atomicite_echec_ecriture_tmp_supprime_ancien_intact);
  RUN_TEST(test_atomicite_puits_reste_sticky_apres_le_premier_echec);
  RUN_TEST(test_atomicite_echec_exactement_a_la_derniere_octet_du_tampon);
  RUN_TEST(test_atomicite_echec_en_cours_de_flux_written_inferieur_a_jsonSize);
  RUN_TEST(test_reconcile_orphelin_efface_ancien_intact);
  RUN_TEST(test_reconcile_sans_orphelin_est_un_no_op);

  RUN_TEST(test_source_flush_compare_stricte_w_expected);
  RUN_TEST(test_source_saveToFile_supprime_le_tmp_jamais_l_ancien_sur_echec);
  RUN_TEST(test_source_reconcile_appelle_remove_seul_sans_open_ni_exists);
  RUN_TEST(test_source_reconcile_appele_dans_begin_avant_loadFromFile);
  RUN_TEST(test_source_avertissements_jamais_error_sur_le_chemin_atomicite);

  return UNITY_END();
}
