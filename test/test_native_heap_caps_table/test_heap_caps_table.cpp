// =============================================================================
// Tests natifs — LA TABLE DE SÉLECTION DES MASQUES DE TAS (feature-122, C-1a)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
//
// CE QUI EST SOUS TEST : la dérivation `(all_caps & caps) == caps` de l'IDF,
// telle que la calcule `heapMaskSelectsHeap()` (src/heap_caps_logic.h) — la
// MÊME fonction que le firmware appelle pour composer les 12 bits `pred`
// publiés par `/get-system-info?hf=1` et épinglés par static_assert dans
// src/heap_metrics.cpp.
//
// ⚠️ POURQUOI CE FICHIER NE RÉIMPLÉMENTE PAS LA RÈGLE (piège de tautologie,
// risque n°1 du lot) : un test qui recalcule `(all_caps & caps) == caps` pour
// se comparer à lui-même ne prouve RIEN. Ici, les 12 attendus sont écrits À LA
// MAIN, un par un, recopiés de la table de la spec en français (« voit » /
// « ignore ») et non calculés. Le seul calcul du fichier est celui de la
// fonction de production.
//
// ⚠️ ET POURQUOI LES SENTINELLES DE CÂBLAGE (bas de fichier) : ce dépôt porte
// déjà un MODÈLE ORPHELIN vivant (`oomProbeTakeTokenModel`, testé 9 fois alors
// qu'aucun chemin du firmware ne l'exécute). Une fonction pure non appelée est
// une fonction dont les tests attestent d'un invariant inexistant. Les
// sentinelles vérifient donc que CHAQUE fonction pure du lot est réellement
// appelée par le firmware, dans la fonction attendue, sur du code DÉPOUILLÉ de
// ses commentaires (forme corrigée de bug-114 : le 07/09/2026, une sentinelle
// de ce dépôt était satisfaite par une simple MENTION en commentaire).
// =============================================================================

#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap_caps_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// 1. LA TABLE DE SÉLECTION — 12 cas (4 masques × 3 tas)
// ===========================================================================
// Les attendus sont des littéraux, recopiés de la table de feature-122 :
//
//   Masque                      | DRAM interne | PSRAM  | Réserve DMA
//   0x0804 INTERNAL|8BIT        | voit         | ignore | VOIT ⚠️ (le piège)
//   0x080C INTERNAL|DMA|8BIT    | voit         | ignore | voit
//   0x1000 DEFAULT              | voit         | VOIT ⚠️ | ignore
//   0x1804 INTERNAL|8BIT|DEFAULT| voit         | ignore | ignore ✅
//
// Le lot entier existe pour documenter la ligne 0x0804 : poser une réserve DMA
// sans changer la métrique ferait afficher aux gardes du projet un gros bloc
// libre — celui de la réserve — pendant que le pool général resterait affamé.

struct SelectionCase {
  uint32_t mask;
  const char* maskLabel;
  uint32_t heapAllCaps;
  const char* heapLabel;
  bool expectedSelects;
  const char* why;
};

static const SelectionCase kTable[12] = {
    // --- 0x0804 : la métrique ACTUELLE de toutes les gardes du projet --------
    {kHeapMaskInternal8, "0x0804 INTERNAL|8BIT", kHeapAllCapsInternalDram, "DRAM interne", true,
     "0x0804 doit voir la DRAM interne : c'est le pool ou malloc()/new puisent."},
    {kHeapMaskInternal8, "0x0804 INTERNAL|8BIT", kHeapAllCapsPsram, "PSRAM", false,
     "0x0804 ne doit PAS voir la PSRAM : elle n'a pas le bit INTERNAL (defini comme "
     "'ne disparait pas quand le cache spiram est coupe'). Le bit 8BIT est COMMUN aux "
     "deux tas : un test satisfait par '!= 0' repondrait oui ici."},
    {kHeapMaskInternal8, "0x0804 INTERNAL|8BIT", kHeapAllCapsDmaReserve, "reserve DMA", true,
     "LE PIEGE DU LOT : 0x0804 VOIT la reserve DMA (all_caps 0x080E porte INTERNAL et "
     "8BIT). Poser une reserve sans toucher a la metrique aveuglerait la garde 503."},

    // --- 0x080C : la signature exacte des échecs observés en production ------
    {kHeapMaskInternalDma8, "0x080C INTERNAL|DMA|8BIT", kHeapAllCapsInternalDram, "DRAM interne",
     true, "0x080C doit voir la DRAM interne : c'est de la que le WiFi sert ses descripteurs."},
    {kHeapMaskInternalDma8, "0x080C INTERNAL|DMA|8BIT", kHeapAllCapsPsram, "PSRAM", false,
     "0x080C ne doit PAS voir la PSRAM : le moteur DMA de l'ESP32 ne sait pas l'adresser. "
     "C'est la raison STRUCTURELLE pour laquelle aucune politique d'allocation ne peut "
     "deplacer les 1 622 o qui echouent (bug-080)."},
    {kHeapMaskInternalDma8, "0x080C INTERNAL|DMA|8BIT", kHeapAllCapsDmaReserve, "reserve DMA",
     true, "0x080C voit la reserve DMA : c'est justement ce qu'une reserve sert a offrir."},

    // --- 0x1000 : DEFAULT seul, ce que demande tout malloc()/new ordinaire ---
    {kHeapMaskDefault, "0x1000 DEFAULT", kHeapAllCapsInternalDram, "DRAM interne", true,
     "0x1000 doit voir la DRAM interne."},
    {kHeapMaskDefault, "0x1000 DEFAULT", kHeapAllCapsPsram, "PSRAM", true,
     "0x1000 voit AUSSI la PSRAM : c'est pourquoi une garde adossee a DEFAULT seul "
     "mesurerait 4 Mo de marge inexistante pour un besoin DMA."},
    {kHeapMaskDefault, "0x1000 DEFAULT", kHeapAllCapsDmaReserve, "reserve DMA", false,
     "0x1000 ne voit PAS la reserve DMA : la region reenregistree n'a pas le bit DEFAULT "
     "(caps[] = {0, DMA|INTERNAL, 8BIT|32BIT})."},

    // --- 0x1804 : la métrique CANDIDATE du lot C-1b (feature-123) -----------
    {kHeapMaskInternal8Default, "0x1804 INTERNAL|8BIT|DEFAULT", kHeapAllCapsInternalDram,
     "DRAM interne", true, "0x1804 doit voir la DRAM interne, sinon la candidate ne mesure rien."},
    {kHeapMaskInternal8Default, "0x1804 INTERNAL|8BIT|DEFAULT", kHeapAllCapsPsram, "PSRAM", false,
     "0x1804 ignore la PSRAM (absence d'INTERNAL cote tas)."},
    {kHeapMaskInternal8Default, "0x1804 INTERNAL|8BIT|DEFAULT", kHeapAllCapsDmaReserve,
     "reserve DMA", false,
     "0x1804 ignore la reserve DMA (absence de DEFAULT cote tas) : c'est TOUT l'interet de "
     "la candidate. Si ce cas passe au vert avec 0x0804 a la place, la mutation du masque "
     "n'a tue personne."},
};

void test_selection_table_twelve_cases(void) {
  for (int i = 0; i < 12; ++i) {
    const SelectionCase& c = kTable[i];
    const bool got = heapMaskSelectsHeap(c.heapAllCaps, c.mask);
    char msg[512];
    snprintf(msg, sizeof(msg), "Cas %d/12 — masque %s vs tas %s (all_caps=0x%04X) : attendu %s. %s",
             i + 1, c.maskLabel, c.heapLabel, (unsigned)c.heapAllCaps,
             c.expectedSelects ? "SELECTIONNE" : "IGNORE", c.why);
    TEST_ASSERT_EQUAL_MESSAGE(c.expectedSelects ? 1 : 0, got ? 1 : 0, msg);
  }
}

// ---------------------------------------------------------------------------
// Les deux cas qui portent tout le dossier, isolés pour qu'un échec se lise
// dans le NOM du test et pas seulement dans un message d'itération.
// ---------------------------------------------------------------------------

void test_the_trap_0x0804_sees_the_dma_reserve(void) {
  TEST_ASSERT_TRUE_MESSAGE(
      heapMaskSelectsHeap(kHeapAllCapsDmaReserve, kHeapMaskInternal8),
      "0x0804 — la metrique de TOUTES les gardes du projet — voit la reserve DMA. C'est le "
      "piege que ce lot documente : la garde 503 cesserait de se declencher et l'appareil "
      "planterait la ou elle protege aujourd'hui.");
}

void test_the_candidate_0x1804_sees_neither_psram_nor_reserve(void) {
  TEST_ASSERT_TRUE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsInternalDram, kHeapMaskInternal8Default),
                           "0x1804 doit voir le pool general.");
  TEST_ASSERT_FALSE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsPsram, kHeapMaskInternal8Default),
                            "0x1804 ne doit PAS voir la PSRAM.");
  TEST_ASSERT_FALSE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsDmaReserve, kHeapMaskInternal8Default),
                            "0x1804 ne doit PAS voir la reserve DMA : c'est la seule raison "
                            "d'etre de la candidate de feature-123.");
}

// ---------------------------------------------------------------------------
// LE MUTANT `== caps` → `!= 0`, pris de face
// ---------------------------------------------------------------------------
// Ces trois cas ont une intersection NON VIDE et pourtant un verdict NÉGATIF :
// c'est exactement ce qu'un `!= 0` inverserait. Sans eux, la mutation
// survivrait à un fichier plein d'assertions positives.

void test_non_empty_intersection_is_not_selection(void) {
  TEST_ASSERT_TRUE_MESSAGE((kHeapAllCapsPsram & kHeapMaskInternal8) != 0u,
                           "Precondition du test : 0x0804 et le tas PSRAM ont bien le bit 8BIT en "
                           "commun (0x0404). Sans intersection non vide, le mutant '!= 0' ne "
                           "serait pas discriminant ici.");
  TEST_ASSERT_FALSE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsPsram, kHeapMaskInternal8),
                            "Intersection non vide n'est PAS selection : il faut TOUS les bits "
                            "demandes. Un '!= 0' rendrait la PSRAM visible depuis 0x0804 — le "
                            "defaut exact qui a rendu quatre gardes aveugles (ADR-0058).");

  TEST_ASSERT_TRUE_MESSAGE((kHeapAllCapsDmaReserve & kHeapMaskInternal8Default) != 0u,
                           "Precondition : 0x1804 et la reserve DMA partagent INTERNAL|8BIT.");
  TEST_ASSERT_FALSE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsDmaReserve, kHeapMaskInternal8Default),
                            "0x1804 exige DEFAULT, que la reserve n'a pas : un '!= 0' ferait voir "
                            "la reserve a la candidate, et la table entiere s'effondrerait.");

  TEST_ASSERT_TRUE_MESSAGE((kHeapAllCapsDmaReserve & kHeapMaskDefault) == 0u,
                           "0x1000 et la reserve n'ont AUCUN bit commun : ce cas-la reste negatif "
                           "meme sous le mutant, d'ou les deux precedents.");
}

void test_spiram_mask_isolates_the_external_heap(void) {
  // A6 : le plancher PSRAM doit être celui du tas PSRAM, et de lui seul —
  // sinon `psram_free` et son plancher ne parleraient pas de la même chose.
  TEST_ASSERT_TRUE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsPsram, kHeapMaskSpiram),
                           "0x0400 doit selectionner le tas PSRAM (A6).");
  TEST_ASSERT_FALSE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsInternalDram, kHeapMaskSpiram),
                            "0x0400 ne doit pas selectionner la DRAM interne.");
  TEST_ASSERT_FALSE_MESSAGE(heapMaskSelectsHeap(kHeapAllCapsDmaReserve, kHeapMaskSpiram),
                            "0x0400 ne doit pas selectionner la reserve DMA (interne).");
}

// ===========================================================================
// 2. LES 12 BITS PUBLIÉS (`pred`) — 749 = 0x02ED
// ===========================================================================
// La disposition est documentée dans heap_metrics.h : bit (masque × 3 + tas),
// masques dans l'ordre des kHeapFloorSlot* 0..3, tas dans l'ordre
// {DRAM interne, PSRAM, réserve DMA}.
//
// ⚠️ Le mot composé ici l'est depuis les ATTENDUS LITTÉRAUX de kTable, PAS
// depuis heapMaskSelectsHeap : sinon on comparerait la fonction à elle-même. Ce
// test dit « la table attendue vaut 0x02ED », le static_assert de
// heap_metrics.cpp dit « la table CALCULÉE vaut 0x02ED ». Les deux ensemble
// ferment la boucle, et c'est le second qui casse le build sous mutation.

void test_expected_table_encodes_to_749(void) {
  uint16_t bits = 0;
  for (int i = 0; i < 12; ++i) {
    if (kTable[i].expectedSelects) {
      const int m = i / 3;  // kTable est rangée masque par masque, 3 tas chacun
      const int h = i % 3;
      bits = (uint16_t)(bits | (uint16_t)(1u << (m * 3 + h)));
    }
  }
  char msg[256];
  snprintf(msg, sizeof(msg),
           "Les 12 attendus de la spec doivent s'encoder en 0x02ED (749), la valeur publiee dans "
           "`hf.pred` par /get-system-info?hf=1 et epinglee par static_assert dans "
           "src/heap_metrics.cpp. Obtenu 0x%04X (%u).",
           (unsigned)bits, (unsigned)bits);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0x02EDu, bits, msg);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(749u, bits, "0x02ED == 749 : le chiffre lisible dans le JSON.");
}

// ===========================================================================
// 3. OUTILLAGE DES SENTINELLES — lecture du dépôt, commentaires DÉPOUILLÉS
// ===========================================================================

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_heap_caps_table");
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

// Retire EN PLACE les commentaires C et C++ (remplacés par des espaces).
// Forme corrigée de bug-114 : sans dépouillement, une sentinelle qui cherche
// `heapMaskSelectsHeap(` est satisfaite par la simple MENTION du nom dans un
// commentaire — et heap_caps_logic.h en contient plusieurs. Une sentinelle
// qu'un commentaire satisfait est PIRE qu'une sentinelle absente : elle atteste
// d'un câblage inexistant, et plus personne ne la rouvre.
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

// Corps { ... } de la fonction dont la signature commence à `sig`.
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

void test_the_comment_stripper_actually_disarms_the_comment_loophole(void) {
  char probe[] =
      "// heapMaskSelectsHeap() dans un commentaire de ligne\n"
      "/* heapMaskSelectsHeap( en bloc */\n"
      "const char* s = \"heapMaskSelectsHeap(\";\n"
      "x = heapMaskSelectsHeap(a, b);\n";
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, countOccurrences(probe, "heapMaskSelectsHeap("),
                                "Le texte BRUT compte 4 occurrences, dont 3 leurres.");
  stripCppComments(probe);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(probe, "heapMaskSelectsHeap("),
                                "Apres depouillement : le litteral de chaine et l'appel reel.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(probe, "= heapMaskSelectsHeap("),
                                "Seul l'appel reel subsiste.");

  // Puis sur un fichier RÉEL de ce lot qui porte exactement le leurre :
  // src/heap_metrics.h NOMME heapMaskSelectsHeap dans un commentaire (« pred =
  // les 12 bits de la DÉRIVATION, calculés par heapMaskSelectsHeap() ») sans
  // jamais l'appeler. Une sentinelle au niveau du FICHIER y serait donc verte
  // pour rien — c'est le défaut démontré le 07/09/2026 (bug-114).
  long len = 0;
  char* raw = readProjectFile("src/heap_metrics.h", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(raw, "src/heap_metrics.h INTROUVABLE.");
  const int rawCount = countOccurrences(raw, "heapMaskSelectsHeap");
  stripCppComments(raw);
  const int codeCount = countOccurrences(raw, "heapMaskSelectsHeap");
  char msg[320];
  snprintf(msg, sizeof(msg),
           "src/heap_metrics.h : %d mention(s) brute(s) de heapMaskSelectsHeap contre %d dans le "
           "CODE. Le depouillement doit toutes les retirer (elles sont en commentaire), sinon une "
           "sentinelle de cablage y serait satisfaite par un commentaire.",
           rawCount, codeCount);
  TEST_ASSERT_TRUE_MESSAGE(rawCount > 0 && codeCount == 0, msg);
  free(raw);
}

// ===========================================================================
// 4. AUCUNE FONCTION PURE DU LOT N'EST ORPHELINE
// ===========================================================================
// Ce dépôt a déjà UN modèle orphelin assumé et documenté
// (`oomProbeTakeTokenModel`, src/oom_guard_logic.h) : 9 tests natifs valident
// une règle qu'aucun chemin du firmware n'exécute, parce que la version réelle
// est atomique. On ne crée pas le dixième. Chaque fonction de heap_caps_logic.h
// doit donc être appelée DANS LA FONCTION DE PRODUCTION attendue — comptage par
// signature, jamais au niveau du fichier (bug-114).

struct WiringSite {
  const char* file;
  const char* sig;    // signature de la fonction de production
  const char* callee; // appel attendu à l'intérieur
  const char* label;
};

void test_every_pure_function_of_the_lot_has_a_real_caller(void) {
  const WiringSite sites[] = {
      {"src/heap_metrics.cpp", "static constexpr uint16_t heapFloorsPredMask(", "heapMaskSelectsHeap(",
       "la table de selection est calculee par la fonction pure, pas recopiee en dur"},
      {"src/heap_metrics.cpp", "HeapFloors heapFloorsRead(", "heapFloorsPredMask(",
       "les 12 bits publies par /get-system-info?hf=1 viennent bien de cette table"},
      {"src/heap_metrics.cpp", "void heapFloorApply(", "heapFloorUpdate(",
       "le repliement par MINIMUM des planchers passe par la fonction pure"},
      {"src/heap_metrics.cpp", "static void heapFetchMinRelaxed(", "heapAtomicFetchMin(",
       "le fetch_min multi-ecrivains (condition 1 du gate) passe par la fonction pure"},
      {"src/heap_metrics.cpp", "HeapProbe heapFloorsSample(", "heapMaskRoundRobinNext(",
       "le tour de role des masques a parcours O(blocs)"},
      {"src/heap_metrics.cpp", "HeapProbe heapFloorsSample(", "heapWalkBucketIndex(",
       "le chronometrage du parcours 0x0804 (condition 5 du gate)"},
      {"src/heap_metrics.cpp", "HeapFloors heapFloorsRead(", "heapWalkMedianUpperUs(",
       "la mediane approchee publiee dans la 2e ligne de journal"},
      {"src/heap_ring.cpp", "void heapRingSample(", "heapCtxDiscoveryWindowActive(",
       "le bit de contexte 'rafale de decouverte HA' (A5)"},
      {"src/main.cpp", "void setup(", "heapPtrIsExternal(",
       "le temoin 4096/4095 (A7) — la seule preuve DIRECTE du camp d'une allocation"},
  };
  const int n = (int)(sizeof(sites) / sizeof(sites[0]));
  for (int i = 0; i < n; ++i) {
    char* code = readProjectCode(sites[i].file);
    char msg[512];
    snprintf(msg, sizeof(msg), "%s INTROUVABLE (site %d).", sites[i].file, i + 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(code, msg);
    char* body = extractFunctionBody(code, sites[i].sig);
    snprintf(msg, sizeof(msg), "Fonction '%s' introuvable dans %s : la sentinelle a perdu sa "
                               "cible, elle ne prouve plus rien (%s).",
             sites[i].sig, sites[i].file, sites[i].label);
    TEST_ASSERT_NOT_NULL_MESSAGE(body, msg);
    const int calls = countOccurrences(body, sites[i].callee);
    snprintf(msg, sizeof(msg),
             "%s doit appeler %s au moins une fois DANS %s — %s. Sinon la fonction pure devient "
             "un MODELE ORPHELIN de plus (cf. oomProbeTakeTokenModel) et ses tests natifs "
             "n'attestent de rien d'execute.",
             sites[i].file, sites[i].callee, sites[i].sig, sites[i].label);
    TEST_ASSERT_TRUE_MESSAGE(calls >= 1, msg);
    free(body);
    free(code);
  }
}

// ===========================================================================
// 4bis. LE TÉMOIN 4096/4095 EST BRANCHÉ SUR LE SEUIL QU'IL ÉPROUVE (A7 / T4)
// ===========================================================================
// La question qui a lancé ce lot : « le correctif B-1 est-il décoratif ? »
// (`psram_free` immobile à 4 180 340 dans 152 relevés). Le témoin y répond en
// allouant et en regardant l'ADRESSE. Pour que sa réponse veuille dire quelque
// chose, trois conditions de CÂBLAGE doivent tenir — et elles sont vérifiables
// ici, hors cible :
//
//  1. le témoin alloue exactement la classe visée par B-1 (kVfsStdioBufferBytes,
//     4 096 o = DEFAULT_FILE_BUFFER_SIZE de vfs_api.cpp) ;
//  2. le témoin de CONTRÔLE alloue exactement le seuil (kPsramExtmemThresholdBytes),
//     donc la plus grande taille qui reste interne dans les DEUX configurations ;
//  3. c'est `malloc()` — JAMAIS `heap_caps_malloc()` avec des caps explicites,
//     qui contournerait la politique `extmem` que le témoin sert à éprouver.
//
// RÈGLE DE L'IDF, relevée au DÉSASSEMBLAGE de la libheap.a effectivement liée
// (heap_caps_malloc_default) : `bgeu limit, size` → si `limit >= size`, la
// première tentative préfère la RAM interne ; sinon elle vise SPIRAM|DEFAULT
// (0x1400) avec repli sur DEFAULT (0x1000). Donc `taille <= seuil ⇒ interne`,
// et 4 096 o avec un seuil de 4 095 part bien en PSRAM d'abord.
//
// ⚠️ CE QUE CE TEST NE PROUVE PAS : le VERDICT. Le témoin ne s'exécute que sur
// cible. La mutation T4 (seuil 4 095 → 4 096) est tuée à la COMPILATION par le
// static_assert de constants.h, dans les deux environnements — ce qui prouve que
// le couple est verrouillé, pas que la politique agit. Le camp réel des 4 096 o
// se lit dans `hf.w96` (/get-system-info?hf=1) ou dans le champ `tem=0x..` de la
// ligne de journal, après déploiement.

void test_the_4096_4095_witness_is_wired_to_the_extmem_threshold(void) {
  char* code = readProjectCode("src/main.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/main.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void setup(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "setup() introuvable.");

  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(body, "heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)") >= 1,
      "La politique doit etre armee avec kPsramExtmemThresholdBytes : c'est CE seuil que le "
      "temoin eprouve.");
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(body, "malloc(kVfsStdioBufferBytes)") >= 1,
      "Le temoin doit allouer kVfsStdioBufferBytes (4 096 o), la classe EXACTE du tampon stdio "
      "que le lot B-1 visait. Une autre taille ne repondrait pas a la question posee.");
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(body, "malloc(kPsramExtmemThresholdBytes)") >= 1,
      "Le temoin de CONTROLE doit allouer exactement le seuil : c'est la plus grande taille qui "
      "reste interne dans les deux configurations, donc le seul controle qui ne bouge pas quand "
      "le seuil bouge.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, countOccurrences(body, "heap_caps_malloc("),
      "Le temoin doit passer par malloc() NU. Des caps explicites contourneraient la politique "
      "`extmem` mise a l'epreuve, et le temoin ne prouverait rien.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "free(probe4096)") >= 1 &&
                               countOccurrences(body, "free(probe4095)") >= 1,
                           "Les deux temoins doivent etre liberes IMMEDIATEMENT : ce lot ne retient "
                           "aucune allocation (instrumentation passive, risque nul).");
  free(body);
  free(code);

  // Le couple seuil/tampon est verrouille par un static_assert : c'est ce qui
  // tue la mutation T4 des la compilation, en natif comme sur cible.
  char* consts = readProjectCode("src/constants.h");
  TEST_ASSERT_NOT_NULL_MESSAGE(consts, "src/constants.h INTROUVABLE.");
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(consts, "static_assert(kPsramExtmemThresholdBytes + 1 == kVfsStdioBufferBytes") >= 1,
      "constants.h doit verrouiller seuil + 1 == tampon stdio. Sans cet assert, remettre le seuil "
      "a 4096 (la valeur d'avant le lot B-1) passerait en silence et rendrait le correctif "
      "inoperant sans que rien ne rougisse.");
  free(consts);
}

// ===========================================================================
// 5. LES BITS SONT ÉPINGLÉS AUX MALLOC_CAP_* DANS UNE UNITÉ FIRMWARE
// ===========================================================================
// heap_caps_logic.h porte des LITTÉRAUX (0x0804…) pour compiler en natif sans
// esp_heap_caps.h. Le seul point de contact avec l'IDF doit donc être un
// static_assert dans une unité de compilation FIRMWARE : si l'IDF déplaçait un
// bit, c'est la COMPILATION qui doit casser, pas un test natif silencieusement
// devenu faux. Cette sentinelle vérifie que ces static_assert existent
// toujours — leur suppression rendrait la table entière indémontrable.

void test_mask_literals_are_pinned_to_idf_constants_in_firmware(void) {
  char* code = readProjectCode("src/heap_metrics.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/heap_metrics.cpp INTROUVABLE.");

  struct Pin {
    const char* needle;
    const char* label;
  };
  const Pin pins[] = {
      {"static_assert(kHeapMaskInternal8 ==", "0x0804 epingle a MALLOC_CAP_INTERNAL|8BIT"},
      {"static_assert(kHeapMaskInternalDma8 ==", "0x080C epingle a INTERNAL|DMA|8BIT"},
      {"static_assert(kHeapMaskDefault ==", "0x1000 epingle a MALLOC_CAP_DEFAULT"},
      {"static_assert(kHeapMaskInternal8Default ==", "0x1804 epingle a INTERNAL|8BIT|DEFAULT"},
      {"static_assert(kHeapMaskSpiram ==", "0x0400 epingle a MALLOC_CAP_SPIRAM"},
      {"static_assert(heapFloorsPredMask() == 0x02EDu", "la table CALCULEE vaut 749"},
  };
  const int n = (int)(sizeof(pins) / sizeof(pins[0]));
  for (int i = 0; i < n; ++i) {
    char msg[384];
    snprintf(msg, sizeof(msg),
             "src/heap_metrics.cpp doit contenir '%s' (%s). Sans ce static_assert, une "
             "divergence avec l'IDF passerait en silence : les tests natifs, qui ne voient que "
             "des litteraux, resteraient verts.",
             pins[i].needle, pins[i].label);
    TEST_ASSERT_TRUE_MESSAGE(countOccurrences(code, pins[i].needle) >= 1, msg);
  }

  // Et les combinaisons MALLOC_CAP_* attendues sont bien celles écrites.
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(code, "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT") >= 1,
      "L'assert de 0x080C doit nommer INTERNAL|DMA|8BIT : c'est la signature exacte des "
      "allocations qui echouent en production.");
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(code, "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT") >= 1,
      "L'assert de 0x1804 doit nommer INTERNAL|8BIT|DEFAULT (la candidate de feature-123).");
  free(code);
}

// ===========================================================================
// 6. T6 — AUCUN `heapProbeForCaps` NEUF HORS DES SITES CONNUS
// ===========================================================================
// L'angle mort signalé par le gate pool-chemistry : `heap_caps_*` s'exécute
// INTERRUPTIONS COUPÉES. Le budget du lot (2 parcours O(blocs) par échantillon,
// soit 1/s) n'est tenu que si personne n'ajoute d'appel ailleurs. Le gate a
// demandé « un test de grep, pas un assert » : un static_assert ne peut pas
// compter des sites d'appel.
//
// SITES LÉGITIMES, et pourquoi :
//   src/heap_metrics.cpp : 1 définition + 3 appels — la référence 0x0804, le
//       masque en tour de rôle, la boucle des deux masques O(régions) ;
//   src/oom_guard.cpp    : 2 appels PRÉEXISTANTS (chemin d'échec d'allocation,
//       bornés par kOomProbeMinIntervalMs, un échantillon par épisode) ;
//   src/heap_metrics.h   : 1 déclaration.
// Tout autre fichier, ou tout compte différent, doit faire ROUGIR : c'est soit
// un parcours de tas neuf sur un chemin chaud, soit un site à requalifier
// explicitement ici.

void test_no_new_heap_probe_call_site(void) {
  struct Allowed {
    const char* file;
    int expected;
    const char* why;
  };
  const Allowed allowed[] = {
      {"src/heap_metrics.cpp", 4,
       "1 definition + 3 appels (reference 0x0804, tour de role, boucle O(regions))"},
      {"src/oom_guard.cpp", 2,
       "2 appels PREEXISTANTS du chemin d'echec, bornes par kOomProbeMinIntervalMs"},
      {"src/heap_metrics.h", 1, "la declaration"},
  };
  const int nAllowed = (int)(sizeof(allowed) / sizeof(allowed[0]));

  for (int i = 0; i < nAllowed; ++i) {
    char* code = readProjectCode(allowed[i].file);
    char msg[512];
    snprintf(msg, sizeof(msg), "%s INTROUVABLE.", allowed[i].file);
    TEST_ASSERT_NOT_NULL_MESSAGE(code, msg);
    const int got = countOccurrences(code, "heapProbeForCaps(");
    snprintf(msg, sizeof(msg),
             "%s : %d occurrence(s) de heapProbeForCaps( dans le CODE, attendu %d (%s). "
             "heap_caps_* s'execute interruptions coupees : un parcours de tas neuf sort du "
             "budget valide par le gate pool-chemistry (1 parcours O(blocs) par seconde). Si "
             "l'ajout est voulu, il doit etre requalifie ICI, pas glisse en silence.",
             allowed[i].file, got, allowed[i].expected, allowed[i].why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(allowed[i].expected, got, msg);
    free(code);
  }

  // Et nulle part ailleurs. Liste des autres fichiers susceptibles d'y toucher :
  // tous ceux qui incluent heap_metrics.h aujourd'hui, plus les chemins chauds.
  const char* forbidden[] = {"src/web_server.cpp",   "src/web_routes_config.cpp",
                             "src/fs_guard.cpp",     "src/heap_ring.cpp",
                             "src/ws_manager.cpp",   "src/main.cpp",
                             "src/oom_guard_logic.h", "src/heap_caps_logic.h",
                             "src/mqtt_manager.cpp", "src/history.cpp",
                             "src/ota_manager.cpp",  "src/logger.cpp",
                             "src/sensors.cpp",      "src/pump_controller.cpp"};
  const int nForbidden = (int)(sizeof(forbidden) / sizeof(forbidden[0]));
  for (int i = 0; i < nForbidden; ++i) {
    char* code = readProjectCode(forbidden[i]);
    if (code == NULL) continue;  // fichier renommé/supprimé : pas l'objet du test
    const int got = countOccurrences(code, "heapProbeForCaps(");
    char msg[512];
    snprintf(msg, sizeof(msg),
             "%s appelle heapProbeForCaps( %d fois : INTERDIT. Un parcours de tas interruptions "
             "coupees sur ce chemin sortirait du budget du gate pool-chemistry. Les seuls sites "
             "legitimes sont les boucles de src/heap_metrics.cpp et le chemin d'echec de "
             "src/oom_guard.cpp (2 appels bornes, prealables a ce lot).",
             forbidden[i], got);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, got, msg);
    free(code);
  }
}

// Le pendant du précédent : la route HTTP de diagnostic ne doit PAS mesurer.
void test_the_http_route_reads_only_stored_atomics(void) {
  char* code = readProjectCode("src/web_routes_config.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/web_routes_config.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "static void handleGetSystemInfo(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "handleGetSystemInfo introuvable.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, countOccurrences(body, "heapProbeForCaps("),
      "handleGetSystemInfo ne doit JAMAIS parcourir le tas : quatre parcours interruptions "
      "coupees par requete, sur async_tcp, seraient poses sur le chemin meme de la famine que "
      "ce lot mesure (le 08/09 a 07:29, une requete de capture a elle-meme declenche un echec "
      "d'allocation).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "heapFloorsRead(") >= 1,
                           "handleGetSystemInfo doit lire l'instantane deja range "
                           "(heapFloorsRead), c'est le seul acces autorise.");
  // Et l'objet `hf` reste CONDITIONNÉ : sans le paramètre, la charge utile est
  // inchangée à l'octet près (964 o) — la route de diagnostic ne doit pas
  // devenir la première victime de la famine qu'elle diagnostique.
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "hasParam(\"hf\")") >= 1,
                           "L'objet `hf` doit rester conditionne a ?hf=1 : 1 741 o par defaut "
                           "contre 964, alors que le pire maxBloc8 observe descend a 852 o. "
                           "Rendre `hf` inconditionnel ouvrirait une bande de fragmentation ou "
                           "cette route echouerait alors qu'elle passait avant le lot.");
  free(body);
  free(code);
}

// ===========================================================================
// 7. AUCUNE GARDE NE BASCULE SUR LES PLANCHERS (le lot MESURE, il ne corrige pas)
// ===========================================================================
// Invariant de périmètre : la bascule éventuelle d'une garde vers 0x1804 est
// feature-123, avec son ADR. Si ce lot déplaçait déjà une garde, un mauvais
// dimensionnement de réserve deviendrait irréversible jusqu'au redémarrage.

void test_no_guard_reads_the_new_floors(void) {
  const char* guards[] = {"src/web_server.cpp", "src/fs_guard.cpp", "src/oom_guard.cpp",
                          "src/ws_manager.cpp"};
  const int n = (int)(sizeof(guards) / sizeof(guards[0]));
  for (int i = 0; i < n; ++i) {
    char* code = readProjectCode(guards[i]);
    if (code == NULL) continue;
    char msg[512];
    snprintf(msg, sizeof(msg),
             "%s lit heapFloorsRead() : INTERDIT dans ce lot. Le refus 503, la garde d'ouverture "
             "de fichiers et le parachute OOM doivent rester sur 0x0804 et "
             "kStaticFileMinAllocBytes. Basculer une garde appartient a feature-123, avec son ADR "
             "— c'est ce decoupage qui evite qu'une reserve mal calibree ferme la porte OTA par "
             "laquelle on viendrait la corriger.",
             guards[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(code, "heapFloorsRead("), msg);
    snprintf(msg, sizeof(msg),
             "%s utilise kHeapMaskInternal8Default (0x1804) : la candidate ne doit alimenter "
             "AUCUNE decision dans ce lot (feature-123).",
             guards[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(code, "kHeapMaskInternal8Default"), msg);
    free(code);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_selection_table_twelve_cases);
  RUN_TEST(test_the_trap_0x0804_sees_the_dma_reserve);
  RUN_TEST(test_the_candidate_0x1804_sees_neither_psram_nor_reserve);
  RUN_TEST(test_non_empty_intersection_is_not_selection);
  RUN_TEST(test_spiram_mask_isolates_the_external_heap);
  RUN_TEST(test_expected_table_encodes_to_749);
  RUN_TEST(test_the_comment_stripper_actually_disarms_the_comment_loophole);
  RUN_TEST(test_every_pure_function_of_the_lot_has_a_real_caller);
  RUN_TEST(test_the_4096_4095_witness_is_wired_to_the_extmem_threshold);
  RUN_TEST(test_mask_literals_are_pinned_to_idf_constants_in_firmware);
  RUN_TEST(test_no_new_heap_probe_call_site);
  RUN_TEST(test_the_http_route_reads_only_stored_atomics);
  RUN_TEST(test_no_guard_reads_the_new_floors);
  return UNITY_END();
}
