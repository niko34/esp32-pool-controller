// =============================================================================
// Tests natifs — SENTINELLES DE STRUCTURE, lot « télémétrie mémoire »
// (feature-127 R1, feature-128 R2/R3)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
//
// POURQUOI CE FICHIER EXISTE (et pas seulement test_native_heap_census_logic) :
// une bonne partie des mutants nommés du lot touche du code IMPUR — le
// callback heap_caps_walk lui-même (src/heap_census.cpp), les routes HTTP
// (src/web_routes_heap.cpp), la sérialisation JSON (src/web_routes_config.cpp),
// l'enregistrement des routes (src/web_server.cpp) — qui inclut Arduino.h,
// ESPAsyncWebServer, esp_heap_caps.h et ne peut PAS compiler sous env:native
// (cf. platformio.ini, build_src_filter de env:native : ces fichiers n'y
// figurent pas et ADR-0017 leur interdit toute dépendance native).
//
// La méthode retenue ici est celle DÉJÀ établie par
// test_native_heap_caps_table (lot C-1a) : lire le fichier SOURCE réel, retirer
// ses commentaires (bug-114 : une sentinelle satisfaite par une MENTION en
// commentaire atteste d'un câblage inexistant), et vérifier des propriétés
// STRUCTURELLES (ordre des lignes, présence/absence d'un motif, nombre
// d'occurrences) — jamais réimplémenter la logique métier elle-même (ce
// qui testerait le test contre lui-même, pas le firmware).
//
// ⚠️ CE QUE CES TESTS NE PROUVENT PAS : le COMPORTEMENT RUNTIME du code impur
// (est-ce que heap_caps_walk se comporte comme prévu, est-ce que l'atomique
// CAS fonctionne sous vraie concurrence FreeRTOS...) — seulement que le TEXTE
// SOURCE porte la forme attendue. C'est un test plus faible qu'une exécution
// réelle, documenté comme tel dans le rapport de test-engineer plutôt que
// présenté comme équivalent. Les propriétés véritablement RUNTIME (S1, S12,
// preuve sur cible) restent du ressort du plan de test manuel.
// =============================================================================

#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// OUTILLAGE (repris de test_native_heap_caps_table, même idiome)
// ===========================================================================

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_heap_telemetry_guard");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i == 0) {
      if (!rooted[0]) continue;
      snprintf(path, sizeof(path), "%s", rooted);
    } else if (i == 1) {
      snprintf(path, sizeof(path), "%s", rel);
    } else {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) continue;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4000000L) {
      fclose(fp);
      continue;
    }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) {
      fclose(fp);
      continue;
    }
    const size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    *outLen = (long)got;
    return buf;
  }
  return NULL;
}

static int countOccurrences(const char* hay, const char* needle) {
  int n = 0;
  const char* p = hay;
  const size_t len = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) {
    ++n;
    p += len;
  }
  return n;
}

// Retire EN PLACE les commentaires C et C++ (remplacés par des espaces) —
// forme corrigée de bug-114, identique à test_native_heap_caps_table.
static void stripCppComments(char* s) {
  bool inLine = false, inBlock = false, inStr = false, inChr = false;
  for (size_t i = 0; s[i] != '\0'; ++i) {
    if (inLine) {
      if (s[i] == '\n') inLine = false;
      else s[i] = ' ';
    } else if (inBlock) {
      if (s[i] == '*' && s[i + 1] == '/') {
        s[i] = ' ';
        s[i + 1] = ' ';
        ++i;
        inBlock = false;
      } else if (s[i] != '\n') {
        s[i] = ' ';
      }
    } else if (inStr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '"') inStr = false;
    } else if (inChr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '\'') inChr = false;
    } else if (s[i] == '/' && s[i + 1] == '/') {
      s[i] = ' ';
      s[i + 1] = ' ';
      ++i;
      inLine = true;
    } else if (s[i] == '/' && s[i + 1] == '*') {
      s[i] = ' ';
      s[i + 1] = ' ';
      ++i;
      inBlock = true;
    } else if (s[i] == '"') {
      inStr = true;
    } else if (s[i] == '\'') {
      inChr = true;
    }
  }
}

static char* readProjectCode(const char* rel) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src != NULL) stripCppComments(src);
  return src;
}

// Corps { ... } de la fonction dont la signature contient `sig`.
static char* extractFunctionBody(const char* src, const char* sig) {
  const char* p = strstr(src, sig);
  if (p == NULL) return NULL;
  const char* open = strchr(p, '{');
  if (open == NULL) return NULL;
  int depth = 0;
  const char* q = open;
  for (; *q != '\0'; ++q) {
    if (*q == '{') {
      ++depth;
    } else if (*q == '}') {
      --depth;
      if (depth == 0) {
        ++q;
        break;
      }
    }
  }
  const size_t n = (size_t)(q - open);
  char* body = (char*)malloc(n + 1);
  if (body == NULL) return NULL;
  memcpy(body, open, n);
  body[n] = '\0';
  return body;
}

// Reprises LITTÉRALES de test_native_heap_floors (même idiome, même fichier
// SOURCE réel analysé) : extraction du format d'un snprintf et de la taille
// du tampon associé, nécessaires à la sentinelle D4-f (lignes « Heap masq
// boot/fen: »). Dupliquées ici plutôt que partagées : chaque binaire de test
// natif est compilé seul (pas de lib commune entre dossiers test_native_*).
static char* extractSnprintfFormat(const char* src, const char* bufName) {
  char needle[128];
  snprintf(needle, sizeof(needle), "snprintf(%s,", bufName);
  const char* p = strstr(src, needle);
  if (p == NULL) return NULL;
  const char* q = strchr(p, '"');
  if (q == NULL) return NULL;
  char* out = (char*)malloc(2048);
  if (out == NULL) return NULL;
  size_t n = 0;
  while (q != NULL && *q == '"') {
    ++q;  // entre dans le littéral
    while (*q != '\0' && *q != '"') {
      if (*q == '\\' && q[1] != '\0') {
        if (n < 2046) out[n++] = *q;
        ++q;
      }
      if (*q == '\0' || *q == '"') break;
      if (n < 2046) out[n++] = *q;
      ++q;
    }
    if (*q == '"') ++q;
    // Littéral suivant ? (concaténation implicite)
    while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') ++q;
    if (*q != '"') break;
  }
  out[n] = '\0';
  return out;
}

static int extractBufferSize(const char* src, const char* name) {
  char needle[128];
  snprintf(needle, sizeof(needle), "char %s[", name);
  const char* p = strstr(src, needle);
  if (p == NULL) return -1;
  p += strlen(needle);
  return atoi(p);
}

// ===========================================================================
// FEATURE-127 (R1) — octets alloués publiés dans `hf`
// ===========================================================================

// MUTANT M1 : sentinelle `if (v == 0)` au lieu de kOomProbeBlockCountUnknown —
// `def`/`spi` rendraient 0 au lieu de null (ADR-0072 violé, ET
// trace_fragmentation.py lirait « zero octet alloue » comme une mesure).
void test_m1_alloc_bytes_helper_uses_sentinel_not_zero(void) {
  char* src = readProjectCode("src/web_routes_config.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/web_routes_config.cpp introuvable.");
  char* body = extractFunctionBody(src, "putHeapAllocatedBytesOrNull(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "putHeapAllocatedBytesOrNull( introuvable dans le code "
                                     "(hors commentaires) — le 4e helper (feature-127) a disparu "
                                     "ou a été renommé.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "== kOomProbeBlockCountUnknown"),
                               "putHeapAllocatedBytesOrNull doit comparer a "
                               "kOomProbeBlockCountUnknown (ADR-0072), jamais a une valeur "
                               "inventee.");
  TEST_ASSERT_NULL_MESSAGE(strstr(body, "== 0)"),
                           "MUTANT M1 : le helper compare a 0 au lieu de "
                           "kOomProbeBlockCountUnknown — def/spi rendraient 0 (une mesure "
                           "credible mais fausse) au lieu de null, et trace_fragmentation.py "
                           "lirait '0 octet alloue' comme une VRAIE mesure au lieu d'une "
                           "absence.");
  free(body);
  free(src);
}

// MUTANT M2 : champ d'octets ramené à 16 bits (262144 tronqué à 65535).
void test_m2_allocated_bytes_fields_are_uint32_not_narrower(void) {
  char* metrics_h = readProjectCode("src/heap_metrics.h");
  TEST_ASSERT_NOT_NULL(metrics_h);
  const char* p1 = strstr(metrics_h, "allocatedBytes;");
  TEST_ASSERT_NOT_NULL_MESSAGE(p1, "champ allocatedBytes introuvable dans HeapProbe "
                                   "(heap_metrics.h).");
  // Cherche le type juste avant le nom du champ, sur la même ligne logique.
  const char* lineStart = p1;
  while (lineStart > metrics_h && lineStart[-1] != '\n') lineStart--;
  char line[256];
  size_t n = (size_t)(p1 - lineStart);
  if (n >= sizeof(line)) n = sizeof(line) - 1;
  memcpy(line, lineStart, n);
  line[n] = '\0';
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, "uint32_t"),
                               "MUTANT M2 : le champ allocatedBytes (HeapProbe) n'est plus "
                               "uint32_t — 262144 o (~256 Ko, ordre de grandeur i8 attendu par "
                               "la spec) tronquerait a 65535 sous uint16_t.");

  const char* p2 = strstr(metrics_h, "lastAllocatedBytes;");
  TEST_ASSERT_NOT_NULL_MESSAGE(p2, "champ lastAllocatedBytes introuvable dans HeapFloorEntry.");
  const char* lineStart2 = p2;
  while (lineStart2 > metrics_h && lineStart2[-1] != '\n') lineStart2--;
  char line2[256];
  size_t n2 = (size_t)(p2 - lineStart2);
  if (n2 >= sizeof(line2)) n2 = sizeof(line2) - 1;
  memcpy(line2, lineStart2, n2);
  line2[n2] = '\0';
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line2, "uint32_t"),
                               "MUTANT M2 (variante HeapFloorEntry) : lastAllocatedBytes doit "
                               "rester uint32_t.");
  free(metrics_h);
}

// MUTANT M3 : second appel à heap_caps_get_info() — le parcours O(blocs) coûte
// déjà cher (wmax=711us, croît avec la fragmentation) ; en ajouter un
// aggraverait ce que l'instrument mesure.
void test_m3_exactly_one_heap_caps_get_info_call_in_whole_project(void) {
  const char* files[] = {"src/heap_metrics.cpp", "src/web_routes_config.cpp",
                          "src/web_routes_heap.cpp", "src/heap_census.cpp"};
  int total = 0;
  int perFileHeapMetrics = 0;
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char* code = readProjectCode(files[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(code, files[i]);
    const int c = countOccurrences(code, "heap_caps_get_info(");
    total += c;
    if (strcmp(files[i], "src/heap_metrics.cpp") == 0) perFileHeapMetrics = c;
    free(code);
  }
  char msg[256];
  snprintf(msg, sizeof(msg),
           "MUTANT M3 : %d appel(s) a heap_caps_get_info() au total sur les 4 fichiers du lot "
           "(attendu EXACTEMENT 1, dans heap_metrics.cpp). feature-127 R1 et feature-128 R2/R3 "
           "doivent tous deux REUTILISER ce meme appel, jamais en ajouter un (I2/A2).",
           total);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, total, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, perFileHeapMetrics,
                                "L'unique appel doit vivre dans heap_metrics.cpp (heapProbeForCaps).");
}

// MUTANT M4 : total_allocated_bytes → total_free_bytes (le champ dupliquerait
// `f`, deux series correlees a 100% — indetectable a l'oeil).
void test_m4_allocated_bytes_reads_total_allocated_not_total_free(void) {
  char* src = readProjectCode("src/heap_metrics.cpp");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(src, "p.allocatedBytes = (uint32_t)info.total_allocated_bytes;"),
      "MUTANT M4 : p.allocatedBytes doit lire info.total_allocated_bytes EXACTEMENT — un "
      "remplacement par info.total_free_bytes ferait dupliquer le champ `f` deja publie, "
      "produisant deux series corrélées à 100% et indétectable à l'oeil dans le JSON.");
  TEST_ASSERT_NULL_MESSAGE(strstr(src, "p.allocatedBytes = (uint32_t)info.total_free_bytes;"),
                           "p.allocatedBytes ne doit JAMAIS lire total_free_bytes.");
  free(src);
}

// ===========================================================================
// FEATURE-128 — invariants structurels de heap_census.cpp
// ===========================================================================

// MUTANT M7 : revalidation contre un parcours FRAIS supprimée — R3 doit
// appeler heap_caps_walk À CHAQUE lecture (servicePeek), jamais réutiliser un
// instantané mis en cache (le recensement R2, sSnapshot).
void test_m7_peek_service_walks_fresh_never_reuses_r2_snapshot(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "void servicePeek(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "servicePeek() introuvable — le service R3 a disparu ou "
                                     "renommé.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "heap_caps_walk("),
                               "MUTANT M7 : servicePeek() n'appelle plus heap_caps_walk — sans "
                               "parcours FRAIS a chaque lecture, un bloc LIBERE puis REALLOUE "
                               "entre deux requetes ferait lire le MAUVAIS contenu, desigant un "
                               "coupable errone.");
  TEST_ASSERT_NULL_MESSAGE(strstr(body, "sSnapshot"),
                           "MUTANT M7 (variante) : servicePeek() ne doit JAMAIS lire "
                           "sSnapshot (le cache du recensement R2) — ce serait reutiliser un "
                           "instantane potentiellement PERIME au lieu d'un parcours frais.");
  free(body);
  free(src);
}

// MUTANT M10 : allocation introduite dans un callback heap_caps_walk — sous
// verrou, interruptions masquées ; risque de récursion
// systemLogger -> broadcastLog -> _heapAllowsPush -> systemLogger si un log
// s'y ajoutait un jour en même temps.
void test_m10_no_allocation_or_string_in_either_walk_callback(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  const char* sigs[2] = {"bool censusWalkCb(", "bool peekWalkCb("};
  const char* forbidden[6] = {"String ", "new ", "malloc(", "std::vector", "push_back", ".reserve("};
  for (int i = 0; i < 2; i++) {
    char* body = extractFunctionBody(src, sigs[i]);
    char msgHead[128];
    snprintf(msgHead, sizeof(msgHead), "%s introuvable.", sigs[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(body, msgHead);
    for (int j = 0; j < 6; j++) {
      const char* hit = strstr(body, forbidden[j]);
      char msg[256];
      snprintf(msg, sizeof(msg),
               "MUTANT M10 : %s contient '%s' — allocation dynamique dans un callback "
               "heap_caps_walk (interruptions masquees, spinlock inter-coeurs).",
               sigs[i], forbidden[j]);
      TEST_ASSERT_NULL_MESSAGE(hit, msg);
    }
    free(body);
  }
  free(src);
}

// MUTANT M13 : compteur de concordance faussé — occupés comptés comme libres
// (ou l'inverse). C'est le SEUL auto-contrôle de l'instrument (A2) : sans lui,
// impossible de savoir si le recensement ment.
void test_m13_used_and_free_block_counters_increment_in_the_right_branch(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "bool censusWalkCb(");
  TEST_ASSERT_NOT_NULL(body);

  const char* ifNotUsed = strstr(body, "if (!block.used) {");
  TEST_ASSERT_NOT_NULL_MESSAGE(ifNotUsed, "Branche 'if (!block.used)' introuvable — structure "
                                          "du callback modifiee.");
  const char* freeIncr = strstr(ifNotUsed, "s->freeBlockCount++;");
  TEST_ASSERT_NOT_NULL_MESSAGE(freeIncr, "s->freeBlockCount++ introuvable apres la branche "
                                         "!block.used.");
  // freeIncr doit être PROCHE de ifNotUsed (dans les ~5 lignes suivantes, pas
  // n'importe où plus loin dans le fichier — sinon on validerait un texte qui
  // existe n'importe où, pas la structure réelle).
  TEST_ASSERT_TRUE_MESSAGE((freeIncr - ifNotUsed) < 80,
                           "s->freeBlockCount++ est trop loin de 'if (!block.used) {' pour "
                           "etre dans la MEME branche : la structure ne colle plus au patron "
                           "attendu.");

  const char* usedIncr = strstr(body, "s->usedBlockCount++;");
  TEST_ASSERT_NOT_NULL_MESSAGE(usedIncr, "s->usedBlockCount++ introuvable.");
  char msg[320];
  snprintf(msg, sizeof(msg),
           "MUTANT M13 : s->usedBlockCount++ doit se trouver APRES la branche 'if "
           "(!block.used) { ... return true; }' (donc dans le cas block.used == true), pas a "
           "l'interieur. Sinon les blocs OCCUPES seraient comptes comme LIBRES (ou "
           "l'inverse) — le seul auto-controle du dispositif (concordance avec fb/ab) "
           "deviendrait aveugle a sa propre erreur.");
  TEST_ASSERT_TRUE_MESSAGE(usedIncr > freeIncr, msg);
  // Un "return true;" doit fermer la branche !used entre les deux compteurs
  // (preuve qu'ils sont dans des branches DISTINCTES, pas juste dans le même
  // bloc séquentiel).
  const char* between = strstr(freeIncr, "return true;");
  TEST_ASSERT_NOT_NULL_MESSAGE(between, "Aucun 'return true;' ne separe les deux branches.");
  TEST_ASSERT_TRUE_MESSAGE(between < usedIncr,
                           "Le 'return true;' qui ferme la branche !used doit precede "
                           "s->usedBlockCount++ : sinon les deux compteurs vivent dans le "
                           "MEME chemin d'execution (M13).");
  free(body);
  free(src);
}

// MUTANT M18 (S8) : R2 lit à `ptr + 8` (donc du CONTENU) au lieu de
// `ptr[0..7]` (canari + alloc_size, poisoning léger).
void test_m18_census_callback_reads_ptr_zero_not_ptr_plus_eight(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "bool censusWalkCb(");
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "memcpy(&head, block.ptr, sizeof(head));"),
                               "MUTANT M18 : censusWalkCb doit lire EXACTEMENT block.ptr (les "
                               "8 premiers octets, canari+alloc_size) — S8 interdit tout "
                               "dereferencement de contenu dans R2.");
  TEST_ASSERT_NULL_MESSAGE(strstr(body, "block.ptr + kHeapPoisonHeadBytes"),
                           "MUTANT M18 (variante) : censusWalkCb ne doit JAMAIS lire "
                           "block.ptr + kHeapPoisonHeadBytes (le PAYLOAD) — cela violerait S8 "
                           "en exposant du contenu via R2, une route sans le regime renforce "
                           "de R3.");
  free(body);
  free(src);
}

// MUTANT M19 (S8) : déréférencer l'en-tête d'un bloc LIBRE (used == false) —
// en poisoning léger, un bloc libre ne porte pas d'en-tête fiable (le tas peut
// y avoir écrit un lien de liste libre TLSF).
void test_m19_census_callback_never_reads_poison_head_of_a_free_block(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "bool censusWalkCb(");
  TEST_ASSERT_NOT_NULL(body);

  const char* memcpyCall = strstr(body, "memcpy(&head, block.ptr, sizeof(head));");
  TEST_ASSERT_NOT_NULL(memcpyCall);
  const char* ifNotUsed = strstr(body, "if (!block.used) {");
  TEST_ASSERT_NOT_NULL(ifNotUsed);
  const char* retAfterFree = strstr(ifNotUsed, "return true;");
  TEST_ASSERT_NOT_NULL(retAfterFree);

  char msg[320];
  snprintf(msg, sizeof(msg),
           "MUTANT M19 : la lecture du poison_head_t (memcpy depuis block.ptr) doit se "
           "trouver APRES le 'return true;' qui ferme la branche '!block.used' — donc "
           "inatteignable pour un bloc LIBRE. Si memcpyCall precede ce return, un bloc libre "
           "serait dereference comme s'il portait un en-tete fiable : en poisoning leger, "
           "cette zone peut contenir un lien de liste libre TLSF, pas un poison_head_t.");
  TEST_ASSERT_TRUE_MESSAGE(memcpyCall > retAfterFree, msg);

  // Même précaution pour peekWalkCb (R3) : la lecture du canari doit suivre
  // `if (!block.used || block.size < kHeapPoisonHeadBytes) return true;`.
  char* peekBody = extractFunctionBody(src, "bool peekWalkCb(");
  TEST_ASSERT_NOT_NULL(peekBody);
  const char* peekGuard = strstr(peekBody, "if (!block.used || block.size < kHeapPoisonHeadBytes) return true;");
  TEST_ASSERT_NOT_NULL_MESSAGE(peekGuard, "peekWalkCb : garde 'bloc libre / trop petit' "
                                          "introuvable sous sa forme attendue.");
  const char* peekMemcpy = strstr(peekBody, "memcpy(&headBefore, block.ptr, sizeof(headBefore));");
  TEST_ASSERT_NOT_NULL(peekMemcpy);
  TEST_ASSERT_TRUE_MESSAGE(peekMemcpy > peekGuard,
                           "MUTANT M19 (variante R3) : peekWalkCb lit le canari AVANT la garde "
                           "used/size — un bloc libre serait dereference.");
  free(peekBody);
  free(body);
  free(src);
}

// MUTANT M21 (C1) : auto-désactivation retirée, ou seuil desserré — la base
// est MOUVANTE (wmax passé de 516µs à 711µs en six jours) : sans coupure
// définitive et sans réarmement, l'instrument continuerait indéfiniment sur
// une estimation périmée.
void test_m21_disable_is_definitive_never_rearmed(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  const int setTrue = countOccurrences(src, "sDisabled.store(true");
  const int setFalse = countOccurrences(src, "sDisabled.store(false");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, setTrue, "sDisabled doit être posé EXACTEMENT une fois "
                                            "(noteWalkDuration, seul site de désactivation).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, setFalse,
                                "MUTANT M21 : sDisabled.store(false) trouvé — la désactivation "
                                "C1 doit être DÉFINITIVE, jamais réarmée (la base de "
                                "calibrage est mouvante, cf. constants.h : un dépassement "
                                "ponctuel ne prouve rien sur le suivant).");

  char* body = extractFunctionBody(src, "void noteWalkDuration(");
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "durationUs > kHeapCensusMaxDurationUs"),
                               "MUTANT M21 (variante) : le seuil de désactivation doit être "
                               "kHeapCensusMaxDurationUs — un autre seuil (ou son inversion) "
                               "désarmerait C1 silencieusement.");
  free(body);
  free(src);
}

// MUTANT M16 : recensement/lecture appelés depuis un chemin PÉRIODIQUE plutôt
// qu'à la demande — seul point d'entrée autorisé : les routes HTTP POST de
// web_routes_heap.cpp (jamais main.cpp, heap_ring.cpp, oom_guard.cpp...).
void test_m16_request_functions_called_only_from_the_http_routes_file(void) {
  const char* files[] = {"src/main.cpp",       "src/heap_ring.cpp",  "src/oom_guard.cpp",
                          "src/heap_metrics.cpp", "src/web_routes_heap.cpp"};
  int censusCalls = 0, peekCalls = 0;
  int censusInRoutes = 0, peekInRoutes = 0;
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char* code = readProjectCode(files[i]);
    if (code == NULL) continue;  // fichier optionnel selon arborescence
    const int c = countOccurrences(code, "heapCensusRequest(");
    const int p = countOccurrences(code, "heapPeekRequest(");
    censusCalls += c;
    peekCalls += p;
    if (strcmp(files[i], "src/web_routes_heap.cpp") == 0) {
      censusInRoutes = c;
      peekInRoutes = p;
    } else {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "MUTANT M16 : heapCensusRequest/heapPeekRequest appelé depuis %s — un chemin "
               "HORS des routes HTTP POST est un déclenchement PÉRIODIQUE ou AUTOMATIQUE, "
               "interdit (le parcours coûte cher et croît avec la fragmentation).",
               files[i]);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, c, msg);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, p, msg);
    }
    free(code);
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, censusInRoutes, "heapCensusRequest doit être appelé "
                                                    "EXACTEMENT une fois, dans web_routes_heap.cpp.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, peekInRoutes, "heapPeekRequest doit être appelé EXACTEMENT "
                                                  "une fois, dans web_routes_heap.cpp.");
  TEST_ASSERT_EQUAL_INT(1, censusCalls);
  TEST_ASSERT_EQUAL_INT(1, peekCalls);
}

// M16 (variante loop) : heapCensusServiceLoop() est appelée sans garde
// d'intervalle (chaque tick — coût nul à l'idle), mais ne doit PAS elle-même
// poser une REQUÊTE : elle ne fait QUE consommer REQUESTED->READY.
void test_m16_service_loop_call_site_is_unconditional_and_unique_in_main(void) {
  char* mainSrc = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL(mainSrc);
  const int callCount = countOccurrences(mainSrc, "heapCensusServiceLoop();");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, callCount, "heapCensusServiceLoop() doit être appelée "
                                              "EXACTEMENT une fois dans main.cpp.");

  // DÉCOR CORRIGÉ : compter les occurrences ne prouve PAS l'absence de garde
  // périodique autour de l'appel (un `if (now - lastX >= interval) { ... }`
  // laisserait le nombre d'occurrences à 1, et ce test serait vert alors que
  // l'appel serait devenu PÉRIODIQUEMENT GATÉ — exactement la classe de
  // régression que M16 vise, à un cran de latence près). On exige donc la
  // ligne EXACTE, au niveau d'indentation du corps de loop() (2 espaces) et
  // SEULE sur sa ligne : un wrapping dans un bloc conditionnel changerait
  // son indentation (4+ espaces) ou son entourage immédiat.
  const char* call = strstr(mainSrc, "heapCensusServiceLoop();");
  TEST_ASSERT_NOT_NULL(call);
  const char* lineStart = call;
  while (lineStart > mainSrc && lineStart[-1] != '\n') lineStart--;
  char line[64];
  size_t n = (size_t)(call - lineStart) + strlen("heapCensusServiceLoop();");
  if (n >= sizeof(line)) n = sizeof(line) - 1;
  memcpy(line, lineStart, n);
  line[n] = '\0';
  TEST_ASSERT_EQUAL_STRING_MESSAGE(
      "  heapCensusServiceLoop();", line,
      "MUTANT M16 (variante gate périodique) : l'appel n'est plus à l'indentation "
      "attendue (2 espaces, top-niveau de loop()) — il a probablement été enveloppé dans "
      "un bloc conditionnel/périodique, ce qui retarderait la réponse à une requête HTTP "
      "de diagnostic jusqu'au prochain tick de ce bloc au lieu du tour de loop suivant.");
  free(mainSrc);
}

// ===========================================================================
// web_server.cpp — enregistrement des routes AVANT la garde 503 (S7/I9/M14)
// ===========================================================================

// MUTANT M14 : route déplacée APRÈS la garde 503 — la route répondrait 503
// sous 8192 o, donc MUETTE precisement sur le plateau a 7924 o observe le
// 13/09 (le seul régime que l'instrument sert à diagnostiquer).
void test_m14_heap_routes_registered_before_the_503_guard(void) {
  char* src = readProjectCode("src/web_server.cpp");
  TEST_ASSERT_NOT_NULL(src);
  const char* setupCall = strstr(src, "setupHeapRoutes(server);");
  TEST_ASSERT_NOT_NULL_MESSAGE(setupCall, "setupHeapRoutes(server) introuvable dans "
                                          "web_server.cpp.");
  const char* wsBegin = strstr(src, "wsManager.begin(server);");
  TEST_ASSERT_NOT_NULL(wsBegin);
  const char* guard = strstr(src, "AsyncURIMatcher::all()");
  TEST_ASSERT_NOT_NULL_MESSAGE(guard, "La garde 503 (AsyncURIMatcher::all()) introuvable — "
                                      "structure de web_server.cpp modifiée.");

  TEST_ASSERT_TRUE_MESSAGE(setupCall < wsBegin,
                           "MUTANT M14 (variante S7) : setupHeapRoutes doit précéder "
                           "wsManager.begin().");
  TEST_ASSERT_TRUE_MESSAGE(
      setupCall < guard,
      "MUTANT M14 : setupHeapRoutes(server) doit être enregistrée AVANT la garde 503 "
      "(AsyncURIMatcher::all()) — _attachHandler (WebServer.cpp) parcourt les handlers dans "
      "l'ORDRE D'ENREGISTREMENT et s'arrête au premier qui matche. Une route après ce point "
      "répondrait 503 sous 8192 o de plus gros bloc, donc MUETTE précisément sur le plateau à "
      "7924 o observé le 13/09 — le régime même que l'instrument sert à diagnostiquer.");
  free(src);
}

// ===========================================================================
// S13 — zéro systemLogger dans tout le code neuf (routes ET loop)
// ===========================================================================
// bug-112 : la fenêtre forensique du journal persistant tombe en ~27
// requêtes — une route de diagnostic ne doit JAMAIS pouvoir la consommer.

void test_s13_zero_systemlogger_in_new_heap_telemetry_code(void) {
  const char* files[] = {"src/web_routes_heap.cpp", "src/heap_census.cpp", "src/heap_census.h"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char* code = readProjectCode(files[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(code, files[i]);
    const int c = countOccurrences(code, "systemLogger");
    char msg[256];
    snprintf(msg, sizeof(msg),
             "S13 : %s contient %d occurrence(s) de systemLogger (hors commentaires) — zéro "
             "attendu, tous les échecs doivent se dire par code HTTP + champ JSON.",
             files[i], c);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, c, msg);
    free(code);
  }
}

// ===========================================================================
// TROISIÈME PASSE — correctifs de revue (B2/B3/M1/G3/S1) — M27-M33
// ===========================================================================

// MUTANT M27a (B3) : l'enveloppe min/max heapStart/heapEnd remplacée par un
// écrasement à chaque bloc — publierait les bornes de la DERNIÈRE région
// visitée pendant que les compteurs agrègent TOUTES les régions
// (esp_heap_caps.h : heapInfo est « la région dans laquelle CE BLOC se
// trouve », pas le tas entier ; l'ESP32 en enregistre plusieurs pour
// INTERNAL|8BIT).
void test_m27a_heap_bounds_are_a_min_max_envelope_not_last_region_overwrite(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "bool censusWalkCb(");
  TEST_ASSERT_NOT_NULL(body);

  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "if (s->regionCount == 1u) {"),
                               "MUTANT M27a : la premiere region doit initialiser "
                               "heapStart/heapEnd sous garde regionCount==1 — introuvable.");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(body, "if (regionStart < s->heapStart) s->heapStart = regionStart;"),
      "MUTANT M27a : l'union MINIMUM (regions suivantes) est absente — un ecrasement "
      "inconditionnel publierait la DERNIERE region au lieu de l'enveloppe de TOUTES les "
      "regions vues, incoherent avec freeBlockCount/usedBlockCount qui agregent tout.");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(body, "if (regionEnd > s->heapEnd) s->heapEnd = regionEnd;"),
      "MUTANT M27a : l'union MAXIMUM (regionEnd) est absente.");
  free(body);
  free(src);
}

// MUTANT M27b (B3) : regionCount retiré — sans lui, impossible de savoir si
// heapStart/heapEnd décrivent une seule région (bornes exactes) ou l'union de
// plusieurs (peut inclure des adresses n'appartenant à AUCUNE région réelle).
void test_m27b_region_count_is_tracked_and_incremented_on_region_change(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "bool censusWalkCb(");
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "s->regionCount++;"),
                               "MUTANT M27b : s->regionCount++ introuvable dans censusWalkCb.");
  free(body);

  char* headerSrc = readProjectCode("src/heap_census.h");
  TEST_ASSERT_NOT_NULL(headerSrc);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(headerSrc, "uint32_t regionCount;"),
      "MUTANT M27b (variante) : le champ regionCount a disparu de HeapCensusSnapshot.");
  free(headerSrc);
  free(src);
}

// MUTANT M27c (B3, variante serviceCensus) : la remise à zéro de
// heapStart/heapEnd/regionCount omise — un parcours SANS AUCUN bloc (tas
// vide, cas d'école) republierait les bornes du recensement PRÉCÉDENT sous un
// `seq` NEUF, un mensonge par omission qu'ADR-0072 interdit précisément.
void test_m27c_service_census_resets_region_envelope_before_each_walk(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "void serviceCensus(");
  TEST_ASSERT_NOT_NULL(body);

  const char* resetStart = strstr(body, "sSnapshot.heapStart = 0;");
  const char* resetEnd = strstr(body, "sSnapshot.heapEnd = 0;");
  const char* resetCount = strstr(body, "sSnapshot.regionCount = 0;");
  TEST_ASSERT_NOT_NULL_MESSAGE(resetStart, "MUTANT M27c : sSnapshot.heapStart = 0 absent de "
                                           "serviceCensus — un parcours sans bloc republierait "
                                           "une valeur PERIMEE sous un seq neuf.");
  TEST_ASSERT_NOT_NULL_MESSAGE(resetEnd, "MUTANT M27c : sSnapshot.heapEnd = 0 absent.");
  TEST_ASSERT_NOT_NULL_MESSAGE(resetCount, "MUTANT M27c : sSnapshot.regionCount = 0 absent.");

  // Doit précéder l'appel heap_caps_walk (donc le parcours part d'un état
  // propre, jamais du résidu du recensement précédent).
  const char* walkCall = strstr(body, "heap_caps_walk(");
  TEST_ASSERT_NOT_NULL(walkCall);
  TEST_ASSERT_TRUE_MESSAGE(resetStart < walkCall && resetEnd < walkCall && resetCount < walkCall,
                           "MUTANT M27c (variante ordre) : la remise a zero doit precede "
                           "l'appel a heap_caps_walk.");
  free(body);
  free(src);
}

// MUTANT M28 (B2) : le WARN one-shot d'auto-désactivation retiré de
// main.cpp — sans lui, R2/R3 se coupent définitivement (C1) en SILENCE, et
// l'opérant ne l'apprend qu'en essuyant un 410 côté route, sans savoir de
// combien le seuil (base MOUVANTE, wmax +38% en six jours) a été franchi.
void test_m28a_main_loop_warns_once_when_census_auto_disables(void) {
  char* src = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(src, "heapCensusDisabledWarned"),
      "MUTANT M28 : le drapeau one-shot heapCensusDisabledWarned a disparu de main.cpp.");
  const char* guard = strstr(src, "if (!heapCensusDisabledWarned && heapCensusIsDisabled()) {");
  TEST_ASSERT_NOT_NULL_MESSAGE(guard, "MUTANT M28 : la garde one-shot n'est plus sous la forme "
                                      "attendue (!warned && isDisabled()).");
  const char* warnCall = strstr(guard, "systemLogger.warning(");
  TEST_ASSERT_NOT_NULL_MESSAGE(warnCall, "MUTANT M28 : aucun systemLogger.warning() a l'interieur "
                                         "de la garde one-shot — le WARN C4 a disparu.");
  free(src);
}

// MUTANT M28 (variante champs JSON) : walk_us/max_walk_us/disabled retirés de
// la réponse — les mêmes 3 champs doivent être posés côté SUCCÈS ET côté
// ERREUR (409/410) : c'est précisément au moment d'un 410 que l'opérant a
// besoin de savoir si C1 en est la cause.
void test_m28b_walk_info_fields_present_and_wired_to_both_success_and_error_paths(void) {
  char* src = readProjectCode("src/web_routes_heap.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "void addCensusWalkInfo(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "addCensusWalkInfo introuvable.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "doc[\"walk_us\"] = heapCensusLastWalkUs();"),
                               "MUTANT M28 : champ walk_us absent.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "doc[\"max_walk_us\"] = heapCensusMaxWalkUs();"),
                               "MUTANT M28 : champ max_walk_us absent.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "doc[\"disabled\"] = heapCensusIsDisabled();"),
                               "MUTANT M28 : champ disabled absent.");
  free(body);

  // Câblage : sendCensusJsonError (chemin ERREUR du GET) doit appeler
  // addCensusWalkInfo — sinon un 410 provoqué par C1 resterait muet.
  char* errBody = extractFunctionBody(src, "void sendCensusJsonError(");
  TEST_ASSERT_NOT_NULL(errBody);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(errBody, "addCensusWalkInfo(doc);"),
                               "MUTANT M28 (variante cablage erreur) : sendCensusJsonError "
                               "n'appelle plus addCensusWalkInfo — un 410/409 redeviendrait "
                               "MUET sur la cause C1.");
  free(errBody);

  // Câblage : le chemin SUCCÈS du GET doit aussi appeler addCensusWalkInfo —
  // au moins 2 appels au total dans le fichier (succès + sendCensusJsonError).
  const int totalCalls = countOccurrences(src, "addCensusWalkInfo(doc);");
  TEST_ASSERT_TRUE_MESSAGE(totalCalls >= 2,
                           "MUTANT M28 (variante cablage succes) : addCensusWalkInfo(doc) doit "
                           "etre appelee au moins 2 fois (chemin succes ET sendCensusJsonError) "
                           "— trouve moins.");
  free(src);
}

// MUTANT M29 (M1) : la réclamation d'un READY périmé retirée — sans elle, un
// POST sans GET de suivi (script interrompu, curl tué) verrouille R2/R3 en
// 409 INDÉFINIMENT, et redémarrer (seule échappatoire) est interdit pendant
// la fenêtre d'observation.
void test_m29_stale_ready_is_reclaimed_by_both_service_census_and_service_peek(void) {
  char* src = readProjectCode("src/heap_census.cpp");
  TEST_ASSERT_NOT_NULL(src);

  const char* sigs[2] = {"void serviceCensus(", "void servicePeek("};
  const char* stateVars[2] = {"sCensusState", "sPeekState"};
  const char* snapVars[2] = {"sSnapshot", "sPeekSnapshot"};
  for (int i = 0; i < 2; i++) {
    char* body = extractFunctionBody(src, sigs[i]);
    char headMsg[128];
    snprintf(headMsg, sizeof(headMsg), "%s introuvable.", sigs[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(body, headMsg);

    char readyCmp[96];
    snprintf(readyCmp, sizeof(readyCmp), "%s.load(std::memory_order_acquire) == kHeapCensusReady &&",
             stateVars[i]);
    const char* readyCheck = strstr(body, readyCmp);
    char msg1[256];
    snprintf(msg1, sizeof(msg1),
             "MUTANT M29 : %s ne reclame plus le READY perime (comparaison '%s' introuvable) — "
             "un POST sans GET de suivi verrouillerait la machine a etats en 409 indefiniment.",
             sigs[i], readyCmp);
    TEST_ASSERT_NOT_NULL_MESSAGE(readyCheck, msg1);

    char staleCall[96];
    snprintf(staleCall, sizeof(staleCall), "heapCensusIsStale(nowMs, %s.tookAtMs, kHeapCensusStaleMs)",
             snapVars[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(readyCheck, staleCall),
                                 "MUTANT M29 (variante) : le test de peremption n'est pas "
                                 "cable sur le bon instantane.");

    const char* reclaimCas = strstr(body, "compare_exchange_strong(stale, kHeapCensusIdle");
    TEST_ASSERT_NOT_NULL_MESSAGE(reclaimCas, "MUTANT M29 (variante CAS) : la transition "
                                             "READY->IDLE de reclamation est absente.");

    // La réclamation doit précéder le refus rapide sur REQUESTED (sinon un
    // état READY périmé resterait bloquant pour la requête EN COURS).
    const char* requestedGuard = strstr(body, "!= kHeapCensusRequested) return;");
    TEST_ASSERT_NOT_NULL(requestedGuard);
    TEST_ASSERT_TRUE_MESSAGE(readyCheck < requestedGuard,
                             "MUTANT M29 (variante ordre) : la reclamation READY perime doit "
                             "avoir lieu AVANT le refus rapide sur l'etat REQUESTED.");
    free(body);
  }
  free(src);
}

// ===========================================================================
// FEATURE-144 (D4) — sentinelles de structure sur src/main.cpp
// ===========================================================================
// ⚠️ check_memory_discipline.sh NE COUVRE PAS main.cpp pour le motif « toute
// String » (ses motifs stricts ne visent que logger.{h,cpp} et
// log_arena_logic.{h,cpp}) : son vert ne prouve RIEN ici. Ces sentinelles
// lisent le texte SOURCE réel (commentaires retirés, bug-114) et vérifient
// des propriétés structurelles — jamais le comportement runtime (impur, hors
// périmètre natif).

// D4-a : le bloc d'émission de la chute (`if (dropResult.emit) { ... }`) ne
// doit contenir NI String, NI concaténation par `+` — ADR-0082, le chemin de
// journalisation n'alloue pas, y compris sur l'instrument qui mesure la
// mémoire elle-même.
void test_d4a_drop_emission_block_has_no_string_allocation(void) {
  char* src = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/main.cpp introuvable.");
  char* body = extractFunctionBody(src, "if (dropResult.emit) {");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      body, "Bloc `if (dropResult.emit) { ... }` introuvable dans src/main.cpp — le détecteur "
            "de chute (feature-144) a disparu ou a été renommé.");
  TEST_ASSERT_NULL_MESSAGE(strstr(body, "String"),
                           "MUTANT ADR-0082 : le bloc d'émission de la chute contient `String` "
                           "— le chemin de journalisation ne doit JAMAIS allouer, y compris "
                           "quand il émet réellement (ironie à ne pas rater : l'instrument qui "
                           "mesure la mémoire ne doit pas en consommer).");
  TEST_ASSERT_NULL_MESSAGE(
      strstr(body, "\" + "),
      "MUTANT ADR-0082 (variante) : concaténation de littéraux par `+` détectée dans le bloc "
      "d'émission — signe d'une construction de String plutôt que d'un snprintf sur tampon de "
      "pile.");
  free(body);
  free(src);
}

// D4-b : le tampon de pile `char ligneChute[96]` doit exister — ⚠️ NOM
// DISTINCT de `ligne` à dessein (test_native_heap_floors::
// test_the_historic_heap_8bits_line_is_untouched identifie la ligne
// historique « Heap 8bits: » par le PREMIER tampon nommé `ligne` du fichier —
// un nouveau tampon nommé `ligne` casserait cette sentinelle AUSSI, en plus
// de celle-ci).
void test_d4b_stack_buffer_ligneChute_96_exists(void) {
  char* src = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(src, "char ligneChute[96];"),
      "Tampon de pile `char ligneChute[96]` introuvable — soit il a été renommé (et risque "
      "d'entrer en collision avec la sentinelle de la ligne historique `ligne`), soit il a été "
      "retaillé sans mise à jour de ce test.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, countOccurrences(src, "char ligne["),
      "Exactement UN tampon nommé `ligne` (la série historique « Heap 8bits: ») doit exister — "
      "`ligneChute`, `ligne2`, `ligne3` ont des noms distincts PAR CONSTRUCTION pour ne jamais "
      "matcher le motif `char ligne[` de test_native_heap_floors.");
  free(src);
}

// D4-c : heapDropEvaluate() appelé EXACTEMENT une fois dans tout le projet —
// un second site d'appel impliquerait un second HeapDropState caché (double
// anti-rafale, double comptage) ou un usage hors de checkHeapHealth().
void test_d4c_heapDropEvaluate_called_exactly_once(void) {
  char* src = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL(src);
  const int c = countOccurrences(src, "heapDropEvaluate(");
  char msg[200];
  snprintf(msg, sizeof(msg),
           "heapDropEvaluate( appelée %d fois dans src/main.cpp (attendu EXACTEMENT 1, dans "
           "checkHeapHealth()).",
           c);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, c, msg);
  free(src);
}

// D4-d : heapLargestFreeBlock8Bit() appelé UNE SEULE fois dans
// checkHeapHealth() (invariant « aucune mesure de tas supplémentaire » — le
// détecteur de chute CONSOMME l'échantillon déjà pris pour l'auto-guérison,
// il n'en prend pas un second).
void test_d4d_heapLargestFreeBlock8Bit_called_once_in_checkHeapHealth(void) {
  char* src = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "void checkHeapHealth() {");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "checkHeapHealth() introuvable dans src/main.cpp.");
  const int c = countOccurrences(body, "heapLargestFreeBlock8Bit(");
  char msg[220];
  snprintf(msg, sizeof(msg),
           "heapLargestFreeBlock8Bit() appelée %d fois dans checkHeapHealth() (attendu "
           "EXACTEMENT 1) — feature-144 ne doit PAS introduire une seconde mesure de tas : le "
           "détecteur de chute consomme l'échantillon DÉJÀ pris pour l'auto-guérison.",
           c);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, c, msg);
  free(body);
  free(src);
}

// D4-e : le site d'appel utilise la mesure FRAÎCHE (`maxAllocHeap`), jamais
// la statique sentinelle-prone `lastHeapMaxAllocSample` (initialisée à
// UINT32_MAX, cf. main.cpp:95) — si elle y entrait, le test natif
// `test_sentinelle_uint32_max_revele_un_debordement_additif` (suite
// test_native_heap_drop_logic) documente le débordement additif que cela
// provoquerait.
void test_d4e_call_site_uses_fresh_sample_not_the_sentinel_prone_static(void) {
  char* src = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL(src);
  char* body = extractFunctionBody(src, "void checkHeapHealth() {");
  TEST_ASSERT_NOT_NULL(body);
  const char* call = strstr(body, "heapDropEvaluate(");
  TEST_ASSERT_NOT_NULL(call);
  // Les ~40 caractères suivant l'appel portent les deux premiers arguments
  // (state, courant) — assez pour y chercher le nom du second argument sans
  // risquer de déborder sur un autre appel plus loin dans le fichier.
  char window[64];
  snprintf(window, sizeof(window), "%.*s", 60, call);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(window, "maxAllocHeap"),
      "MUTANT sentinelle : heapDropEvaluate() doit recevoir `maxAllocHeap` (mesure fraîche de "
      "ce tick) en second argument.");
  TEST_ASSERT_NULL_MESSAGE(
      strstr(window, "lastHeapMaxAllocSample"),
      "MUTANT sentinelle : heapDropEvaluate() ne doit JAMAIS recevoir `lastHeapMaxAllocSample` "
      "— cette statique vaut UINT32_MAX avant le premier échantillon (sentinelle "
      "« pas encore mesuré »), et la feeder directement provoquerait le débordement additif "
      "documenté par test_sentinelle_uint32_max_revele_un_debordement_additif.");
  free(body);
  free(src);
}

// D4-f : les lignes de référence « Heap 8bits: » (déjà protégée par
// test_native_heap_floors) ET « Heap masq boot/fen: » (152+ relevés
// historiques, AUCUNE sentinelle existante avant feature-144) doivent rester
// INTOUCHÉES mot pour mot — feature-144 ne doit RIEN changer à ces deux
// lignes voisines.
void test_d4f_heap_masq_boot_et_fen_lines_untouched(void) {
  char* src = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL(src);

  // ⚠️ Ces deux formats sont écrits en DEUX littéraux adjacents dans le
  // source (concaténation implicite du C — cf. main.cpp autour des lignes
  // 1371-1373 et 1402-1404) : une recherche de sous-chaîne brute sur le texte
  // SOURCE ne matcherait jamais le format complet (la coupure tombe entre
  // deux guillemets). `extractSnprintfFormat` reconstruit la forme LOGIQUE en
  // recollant les littéraux adjacents — seule méthode fiable ici, déjà
  // établie par test_native_heap_floors pour la ligne historique voisine.
  const char* attenduBoot =
      "Heap masq boot: m804=%u/%u c=0x%02X m80C=%u/%u c=0x%02X "
      "m1804=%u/%u c=0x%02X mPS=%u tem=0x%02X";
  const char* attenduFen =
      "Heap masq fen: m804=%u/%u m80C=%u/%u m1804=%u/%u ctxVu=0x%02X "
      "walk=%u/%uus b804=%u/%u";

  char* fmtBoot = extractSnprintfFormat(src, "ligne2");
  TEST_ASSERT_NOT_NULL_MESSAGE(fmtBoot, "Format du snprintf de `ligne2` introuvable.");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(attenduBoot, fmtBoot,
                                   "`ligne2` (Heap masq boot:) doit rester EXACTEMENT la série "
                                   "historique.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(160, extractBufferSize(src, "ligne2"),
                                "`ligne2` doit rester un char[160].");

  char* fmtFen = extractSnprintfFormat(src, "ligne3");
  TEST_ASSERT_NOT_NULL_MESSAGE(fmtFen, "Format du snprintf de `ligne3` introuvable.");
  TEST_ASSERT_EQUAL_STRING_MESSAGE(attenduFen, fmtFen,
                                   "`ligne3` (Heap masq fen:) doit rester EXACTEMENT la série "
                                   "historique.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(176, extractBufferSize(src, "ligne3"),
                                "`ligne3` doit rester un char[176].");
  free(src);
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_m1_alloc_bytes_helper_uses_sentinel_not_zero);
  RUN_TEST(test_m2_allocated_bytes_fields_are_uint32_not_narrower);
  RUN_TEST(test_m3_exactly_one_heap_caps_get_info_call_in_whole_project);
  RUN_TEST(test_m4_allocated_bytes_reads_total_allocated_not_total_free);

  RUN_TEST(test_m7_peek_service_walks_fresh_never_reuses_r2_snapshot);
  RUN_TEST(test_m10_no_allocation_or_string_in_either_walk_callback);
  RUN_TEST(test_m13_used_and_free_block_counters_increment_in_the_right_branch);
  RUN_TEST(test_m18_census_callback_reads_ptr_zero_not_ptr_plus_eight);
  RUN_TEST(test_m19_census_callback_never_reads_poison_head_of_a_free_block);
  RUN_TEST(test_m21_disable_is_definitive_never_rearmed);
  RUN_TEST(test_m16_request_functions_called_only_from_the_http_routes_file);
  RUN_TEST(test_m16_service_loop_call_site_is_unconditional_and_unique_in_main);

  RUN_TEST(test_m14_heap_routes_registered_before_the_503_guard);

  RUN_TEST(test_s13_zero_systemlogger_in_new_heap_telemetry_code);

  RUN_TEST(test_m27a_heap_bounds_are_a_min_max_envelope_not_last_region_overwrite);
  RUN_TEST(test_m27b_region_count_is_tracked_and_incremented_on_region_change);
  RUN_TEST(test_m27c_service_census_resets_region_envelope_before_each_walk);
  RUN_TEST(test_m28a_main_loop_warns_once_when_census_auto_disables);
  RUN_TEST(test_m28b_walk_info_fields_present_and_wired_to_both_success_and_error_paths);
  RUN_TEST(test_m29_stale_ready_is_reclaimed_by_both_service_census_and_service_peek);

  // feature-144 (D4) — sentinelles de structure sur src/main.cpp
  RUN_TEST(test_d4a_drop_emission_block_has_no_string_allocation);
  RUN_TEST(test_d4b_stack_buffer_ligneChute_96_exists);
  RUN_TEST(test_d4c_heapDropEvaluate_called_exactly_once);
  RUN_TEST(test_d4d_heapLargestFreeBlock8Bit_called_once_in_checkHeapHealth);
  RUN_TEST(test_d4e_call_site_uses_fresh_sample_not_the_sentinel_prone_static);
  RUN_TEST(test_d4f_heap_masq_boot_et_fen_lines_untouched);

  return UNITY_END();
}
