// =============================================================================
// IMPORT D'HISTORIQUE PAR LOTS — bug-101 (plafond 16 Ko) + bug-113 (tamponnage
// JSON non authentifié). Suite native, ADR-0017.
// =============================================================================
// CE QUE CE FICHIER PROTÈGE, ET DANS QUEL ORDRE DE GRAVITÉ
//
//   1. `historyImportModeIsReplace()` — LE point où une erreur DÉTRUIT des
//      données. Un défaut `append` transforme « je restaure ma sauvegarde » en
//      « je la mélange à l'historique vivant », sans le dire. Un défaut
//      `replace` sur un lot de SUITE efface les lots précédents : c'est le
//      piège qui détruisait l'historique de ceux qui découpaient à la main.
//   2. `historyImportSameSlot()` — la règle de collision. Elle doit couvrir RAW,
//      que `dedupAggregatedPoints()` EXCLUT explicitement (history.cpp:1386,
//      `if (p.granularity != RAW)`). Sans elle, un lot RAW réenvoyé — reprise
//      après coupure, double clic — DOUBLE en silence.
//   3. `historyImportTrimPlan()` — le bornage. Soustractions gardées : un
//      `raw - kMaxRawDataPoints` non gardé sur un size_t rend ~1,8e19, donc une
//      PURGE TOTALE de l'historique.
//   4. L'IDEMPOTENCE EXACTE de la fusion, prouvée sur un banc qui délègue
//      toutes ses décisions aux trois fonctions ci-dessus.
//   5. Le CÂBLAGE — que le firmware appelle bien ces règles, au bon endroit, et
//      que le budget de lot du client reste sous le plafond de la route.
//
// POURQUOI DES SENTINELLES DE TEXTE, ET LEUR LIMITE ASSUMÉE
// `history.cpp`, `web_helpers.cpp` et `data/app.js` ne compilent pas en natif
// (FreeRTOS, LittleFS, navigateur). Les invariants de câblage sont donc lus dans
// le SOURCE. C'est faible, et une seule chose les rend honnêtes :
//
//   ⚠️ TOUTE lecture passe par `readProjectCode()`, qui DÉPOUILLE les
//   commentaires, et vise chaque point DANS LE CORPS DE SA FONCTION, extrait
//   par signature.
//
// Ce n'est pas un raffinement. Le 07/09/2026, la sentinelle de
// `test_native_temp_compensation` était satisfaite par une simple MENTION de
// `clampTempCalOffset()` dans un commentaire : supprimer l'appel réel laissait
// la suite VERTE. Un test qu'un commentaire satisfait est pire qu'un test
// absent — il atteste un invariant inexistant, et personne ne le rouvre. Le
// méta-test `test_outillage_*` vérifie l'outil AVANT que les sentinelles s'en
// servent.
//
// ANGLE MORT CONNU : l'extraction reste textuelle. Elle vise la distraction, pas
// la malveillance. Ne pas essayer de parser du C++ ni du JS ici.
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <vector>
#include <algorithm>

#include "history_import_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Granularités — valeurs de l'enum `Granularity` de src/history.h. Recopiées
// (et non incluses : history.h tire FS.h/freertos) puis REVÉRIFIÉES contre le
// source par test_sentinelle_enum_granularite_inchange().
static const uint8_t GRAN_RAW = 0;
static const uint8_t GRAN_HOURLY = 1;
static const uint8_t GRAN_DAILY = 2;

// Masques de dosage — kHistDose* de src/history.h, mêmes précautions.
static const uint8_t MASK_PH = 0x01;
static const uint8_t MASK_UNKNOWN = 0x04;

// =============================================================================
// PARTIE 0 — OUTILLAGE DES SENTINELLES (et son méta-test)
// =============================================================================

// Racine du dépôt reconstruite depuis __FILE__, avec repli sur des préfixes
// relatifs : `pio test` ne garantit pas le répertoire courant.
static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_history_import");
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
    if (len <= 0 || len > 8000000L) { fclose(fp); continue; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(fp); continue; }
    const size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    if (outLen != NULL) *outLen = (long)got;
    return buf;
  }
  return NULL;
}

// Retire EN PLACE les commentaires ligne et bloc (remplacés par des espaces :
// longueurs et numéros de ligne préservés). Les littéraux de chaîne sont
// TRAVERSÉS sans être modifiés — y compris les gabarits `backtick` de
// JavaScript, sans quoi le `//` d'une URL dans un gabarit d'app.js ouvrirait un
// faux commentaire et effacerait la moitié du fichier.
static void stripComments(char* s) {
  bool inLine = false, inBlock = false, inStr = false, inChr = false, inTpl = false;
  for (size_t i = 0; s[i] != '\0'; ++i) {
    if (inLine) {
      if (s[i] == '\n') inLine = false; else s[i] = ' ';
    } else if (inBlock) {
      if (s[i] == '*' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = false; }
      else if (s[i] != '\n') s[i] = ' ';
    } else if (inStr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '"') inStr = false;
    } else if (inChr) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '\'') inChr = false;
    } else if (inTpl) {
      if (s[i] == '\\' && s[i + 1] != '\0') ++i;
      else if (s[i] == '`') inTpl = false;
    } else if (s[i] == '/' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inLine = true; }
    else if (s[i] == '/' && s[i + 1] == '*') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = true; }
    else if (s[i] == '"') inStr = true;
    else if (s[i] == '\'') inChr = true;
    else if (s[i] == '`') inTpl = true;
  }
}

// ⚠️ TOUTE sentinelle de ce fichier lit par ICI, jamais le texte brut.
static char* readProjectCode(const char* rel) {
  char* src = readProjectFile(rel, NULL);
  if (src != NULL) stripComments(src);
  return src;
}

static int countOccurrences(const char* hay, const char* needle) {
  if (hay == NULL || needle == NULL || needle[0] == '\0') return 0;
  int n = 0;
  const size_t len = strlen(needle);
  for (const char* p = strstr(hay, needle); p != NULL; p = strstr(p + len, needle)) ++n;
  return n;
}

// Corps d'une fonction, accolades appariées, à partir de sa SIGNATURE.
// Viser une fonction plutôt que le fichier entier est ce qui empêche une
// sentinelle d'être satisfaite par un appel présent ailleurs dans le fichier :
// `historyImportSameSlot(` existerait aussi si on l'appelait depuis
// `consolidateData()`, ce qui ne prouverait RIEN sur `appendData()`.
static char* extractFunctionBody(const char* src, const char* sig) {
  if (src == NULL) return NULL;
  const char* p = strstr(src, sig);
  if (p == NULL) return NULL;
  const char* open = strchr(p, '{');
  if (open == NULL) return NULL;
  int depth = 0;
  const char* q = open;
  for (; *q != '\0'; ++q) {
    if (*q == '{') ++depth;
    else if (*q == '}') { --depth; if (depth == 0) { ++q; break; } }
  }
  const size_t n = (size_t)(q - open);
  char* body = (char*)malloc(n + 1);
  if (body == NULL) return NULL;
  memcpy(body, open, n);
  body[n] = '\0';
  return body;
}

// Lit le corps d'une fonction d'un fichier DÉPOUILLÉ, en échouant fort si l'un
// ou l'autre manque : une sentinelle aveugle doit rougir, pas passer.
static char* codeOfFunction(const char* rel, const char* sig) {
  char* src = readProjectCode(rel);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "Fichier du depot introuvable : la sentinelle "
      "serait AVEUGLE. Lancer `pio test -e native` depuis la racine du depot.");
  char* body = extractFunctionBody(src, sig);
  free(src);
  if (body == NULL) {
    char msg[400];
    snprintf(msg, sizeof(msg),
        "Signature introuvable dans %s : \"%s\". Si la fonction a ete RENOMMEE, "
        "corriger la signature ICI (et verifier que l'invariant tient toujours) ; "
        "si elle a ete SUPPRIMEE, l'invariant qu'elle portait a disparu avec elle.",
        rel, sig);
    TEST_FAIL_MESSAGE(msg);
  }
  return body;
}

// Extrait le premier entier qui suit `prefix` (dans du code dépouillé).
// Rend -1 si le motif est absent.
static long numberAfter(const char* code, const char* prefix) {
  if (code == NULL) return -1;
  const char* p = strstr(code, prefix);
  if (p == NULL) return -1;
  p += strlen(prefix);
  while (*p == ' ' || *p == '=' || *p == '\t') ++p;
  if (*p < '0' || *p > '9') return -1;
  return strtol(p, NULL, 10);
}

// ---------------------------------------------------------------------------
// MÉTA-TESTS DE L'OUTILLAGE — ils tournent AVANT les sentinelles.
// ---------------------------------------------------------------------------
static void test_outillage_le_depouillement_desarme_la_faille_du_commentaire(void) {
  char probe[] =
      "// bug-101 : historyImportSameSlot() est la regle de collision\n"
      "/* historyImportSameSlot( en bloc */\n"
      "const char* s = \"historyImportSameSlot(\";\n"
      "if (historyImportSameSlot(a, b, c, d)) x = 1;\n";
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, countOccurrences(probe, "historyImportSameSlot("),
      "Le texte BRUT compte 4 occurrences dont 3 leurres : c'est exactement ce "
      "qui a rendu verte une sentinelle vide le 07/09/2026.");
  stripComments(probe);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(probe, "historyImportSameSlot("),
      "Apres depouillement : le litteral de chaine et l'appel reel. Les "
      "commentaires LIGNE et BLOC doivent tous deux disparaitre.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(probe, "(historyImportSameSlot("),
      "Seul l'APPEL reel subsiste une fois l'appel distingue du litteral.");
}

static void test_outillage_les_gabarits_backtick_ne_sont_pas_pris_pour_des_commentaires(void) {
  // Régression propre à data/app.js : un gabarit `…${x}…` contenant `//`
  // (une URL, un chemin) ouvrirait un faux commentaire de LIGNE et effacerait
  // tout le reste de la ligne — y compris du code que la sentinelle cherche.
  char probe[] = "const u = `http://x/y`; const budget = 7168;\n";
  stripComments(probe);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(probe, "const budget = 7168;"),
      "Le code APRES un gabarit backtick contenant `//` doit survivre au "
      "depouillement, sinon la sentinelle app.js lit un fichier tronque.");
}

static void test_outillage_extraction_par_signature_isole_bien_la_fonction(void) {
  const char probe[] =
      "void avant() { CIBLE(1); }\n"
      "void milieu(int a) { if (a) { CIBLE(2); } }\n"
      "void apres() { CIBLE(3); }\n";
  char* body = extractFunctionBody(probe, "void milieu(");
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(body, "CIBLE("),
      "L'extraction doit rendre le corps de `milieu` SEUL : les appels des "
      "fonctions voisines ne doivent pas satisfaire une sentinelle qui vise "
      "milieu (accolades imbriquees comprises).");
  free(body);
  TEST_ASSERT_NULL_MESSAGE(extractFunctionBody(probe, "void absente("),
      "Une signature absente doit rendre NULL, pour que la sentinelle ECHOUE "
      "au lieu de tester le vide.");
}

// =============================================================================
// PARTIE 1 — historyImportModeIsReplace : LE POINT OÙ UNE ERREUR PERD DES DONNÉES
// =============================================================================

static void test_mode_absent_vaut_remplacer(void) {
  TEST_ASSERT_TRUE_MESSAGE(historyImportModeIsReplace(NULL),
      "mode ABSENT doit valoir `replace` : c'est la retrocompatibilite stricte "
      "de la route. Tout client anterieur a v2.48.0 postait un remplacement "
      "total sans envoyer aucun champ `mode`.");
}

static void test_mode_vide_vaut_remplacer(void) {
  TEST_ASSERT_TRUE_MESSAGE(historyImportModeIsReplace(""),
      "mode \"\" doit valoir `replace` : une chaine vide n'est pas une demande "
      "d'ajout.");
}

static void test_mode_replace_vaut_remplacer(void) {
  TEST_ASSERT_TRUE_MESSAGE(historyImportModeIsReplace("replace"), "\"replace\" -> replace.");
}

static void test_mode_append_est_le_seul_a_ajouter(void) {
  TEST_ASSERT_FALSE_MESSAGE(historyImportModeIsReplace("append"),
      "\"append\" est la SEULE valeur qui ajoute. Si elle remplacait, chaque lot "
      "de suite effacerait le precedent : une restauration de 339 points ne "
      "laisserait que son DERNIER lot (~45 points) — le defaut exact de bug-101.");
}

static void test_mode_append_toutes_casses_ascii_ajoutent(void) {
  // EXHAUSTIF : les 64 combinaisons de casse de « append ». La tolerance de
  // casse va dans le sens de la surete — « Append » interprete en `replace`
  // ECRASERAIT l'historique vivant de quelqu'un qui demandait un ajout.
  const char* base = "append";
  for (int mask = 0; mask < 64; ++mask) {
    char v[8];
    for (int i = 0; i < 6; ++i) {
      v[i] = (mask & (1 << i)) ? (char)(base[i] - 'a' + 'A') : base[i];
    }
    v[6] = '\0';
    char msg[220];
    snprintf(msg, sizeof(msg),
        "\"%s\" doit AJOUTER : la casse d'un champ de protocole ne doit jamais "
        "faire basculer vers l'ECRASEMENT de l'historique vivant.", v);
    TEST_ASSERT_FALSE_MESSAGE(historyImportModeIsReplace(v), msg);
  }
}

static void test_mode_inconnu_retombe_sur_remplacer(void) {
  // ⚠️ Le sens de ce repli est DÉLIBÉRÉ et asymétrique : un client qu'on n'a pas
  // compris obtient le comportement HISTORIQUE, jamais une fusion silencieuse.
  const char* inconnus[] = {
    "apend", "add", "ADD", "merge", "concat", "appendx", "append ", " append",
    "app end", "appen", "REPLACE", "Replace", "replac", "true", "1", "0",
    "null", "append\t", "\tappend", "-append", "append,append", "a", "z",
  };
  for (size_t i = 0; i < sizeof(inconnus) / sizeof(inconnus[0]); ++i) {
    char msg[260];
    snprintf(msg, sizeof(msg),
        "\"%s\" est INCONNU (ni vide, ni \"append\" a la casse pres) et doit "
        "retomber sur `replace`. Un inconnu lu comme `append` melangerait la "
        "sauvegarde a l'historique vivant sans le dire a personne.", inconnus[i]);
    TEST_ASSERT_TRUE_MESSAGE(historyImportModeIsReplace(inconnus[i]), msg);
  }
}

static void test_mode_aucune_mutation_d_un_caractere_d_append_n_ajoute(void) {
  // Balayage exhaustif : « append » avec UN caractere remplace par n'importe
  // quel autre octet ASCII imprimable. Aucune de ces 6 x 94 variantes ne doit
  // ajouter (hors la casse, deja couverte). Vise les comparaisons partielles
  // (strncmp trop court, prefixe seul).
  const char* base = "append";
  for (int pos = 0; pos < 6; ++pos) {
    for (int c = 32; c < 127; ++c) {
      char v[8];
      memcpy(v, base, 7);
      v[pos] = (char)c;
      if (v[pos] == base[pos] || v[pos] == (char)(base[pos] - 'a' + 'A')) continue;
      char msg[260];
      snprintf(msg, sizeof(msg),
          "\"%s\" (position %d mutee) n'est PAS \"append\" et doit rendre "
          "`replace`. Une comparaison de prefixe trop courte le laisserait "
          "passer en ajout.", v, pos);
      TEST_ASSERT_TRUE_MESSAGE(historyImportModeIsReplace(v), msg);
    }
  }
}

static void test_mode_est_une_fonction_pure_donc_stable(void) {
  // Aucun etat cache : le meme appel repete rend le meme verdict. Une bascule
  // au second appel (compteur, static) serait indetectable en production et
  // catastrophique sur un import multi-lots, ou la fonction est appelee une
  // fois PAR LOT.
  for (int i = 0; i < 8; ++i) {
    TEST_ASSERT_FALSE(historyImportModeIsReplace("append"));
    TEST_ASSERT_TRUE(historyImportModeIsReplace(NULL));
    TEST_ASSERT_TRUE(historyImportModeIsReplace("replace"));
  }
}

// =============================================================================
// PARTIE 2 — historyImportSameSlot : la collision, RAW COMPRIS
// =============================================================================

static void test_sameslot_meme_creneau_toutes_granularites(void) {
  const uint8_t grans[3] = {GRAN_RAW, GRAN_HOURLY, GRAN_DAILY};
  for (int i = 0; i < 3; ++i) {
    char msg[240];
    snprintf(msg, sizeof(msg),
        "Meme timestamp et meme granularite (%u) = MEME creneau. La granularite "
        "RAW (0) EN FAIT PARTIE : dedupAggregatedPoints() l'exclut "
        "(history.cpp:1386), donc si SameSlot l'excluait aussi, plus rien ne "
        "dedoublonnerait les 96 points RAW d'un export reenvoye.", grans[i]);
    TEST_ASSERT_TRUE_MESSAGE(
        historyImportSameSlot(1757068200u, grans[i], 1757068200u, grans[i]), msg);
  }
}

static void test_sameslot_couvre_explicitement_raw(void) {
  // Test dedie, nomme pour etre lu dans le rapport d'echec : c'est le mutant
  // n°2 du lot (ajout d'un `if (gran != RAW)` dans SameSlot) qui meurt ici.
  TEST_ASSERT_TRUE_MESSAGE(
      historyImportSameSlot(1757068200u, GRAN_RAW, 1757068200u, GRAN_RAW),
      "DEUX POINTS RAW DE MEME TIMESTAMP SONT UN DOUBLON. Exclure RAW ici "
      "reproduit le defaut de doublons MUETS : une reprise apres coupure reseau "
      "reenvoie le lot, chaque point RAW est recree, et l'historique double.");
}

static void test_sameslot_granularites_differentes_au_meme_timestamp(void) {
  // Cas REEL : un creneau minuit porte a la fois un HOURLY et un DAILY de meme
  // timestamp. Les confondre ferait ecraser la moyenne du jour par celle d'une
  // heure — une donnee fausse, et durable (75 jours).
  TEST_ASSERT_FALSE_MESSAGE(
      historyImportSameSlot(1757030400u, GRAN_HOURLY, 1757030400u, GRAN_DAILY),
      "HOURLY et DAILY de meme timestamp (minuit) sont DEUX creneaux distincts.");
  TEST_ASSERT_FALSE(historyImportSameSlot(1757030400u, GRAN_RAW, 1757030400u, GRAN_HOURLY));
  TEST_ASSERT_FALSE(historyImportSameSlot(1757030400u, GRAN_RAW, 1757030400u, GRAN_DAILY));
}

static void test_sameslot_creneaux_voisins_sont_distincts(void) {
  const uint32_t t = 1757068200u;
  TEST_ASSERT_FALSE_MESSAGE(historyImportSameSlot(t, GRAN_RAW, t + 300u, GRAN_RAW),
      "Deux RAW espaces de 5 min sont deux MESURES REELLES. Une tolerance "
      "temporelle ici PERDRAIT une mesure a chaque import.");
  TEST_ASSERT_FALSE_MESSAGE(historyImportSameSlot(t, GRAN_RAW, t + 1u, GRAN_RAW),
      "Une seconde d'ecart suffit : l'egalite est STRICTE, pas approchee.");
  TEST_ASSERT_FALSE(historyImportSameSlot(t, GRAN_HOURLY, t + 3600u, GRAN_HOURLY));
  TEST_ASSERT_FALSE(historyImportSameSlot(t, GRAN_DAILY, t + 86400u, GRAN_DAILY));
}

static void test_sameslot_matrice_exhaustive_et_symetrique(void) {
  // 3 timestamps x 3 granularites = 9 creneaux, 81 paires. Vrai SI ET SEULEMENT
  // SI c'est le meme creneau, et la relation est SYMETRIQUE (l'ordre des
  // arguments ne doit rien changer : la coquille appelle avec l'entrant en
  // premier, un futur refactor pourrait inverser).
  const uint32_t ts[3] = {0u, 1757068200u, 4294967295u};  // dont les deux bornes
  const uint8_t gr[3] = {GRAN_RAW, GRAN_HOURLY, GRAN_DAILY};
  for (int a = 0; a < 9; ++a) {
    for (int b = 0; b < 9; ++b) {
      const bool expected = (a == b);
      const bool got = historyImportSameSlot(ts[a / 3], gr[a % 3], ts[b / 3], gr[b % 3]);
      char msg[240];
      snprintf(msg, sizeof(msg),
          "Creneau (ts=%u,gran=%u) vs (ts=%u,gran=%u) : attendu %s.",
          ts[a / 3], gr[a % 3], ts[b / 3], gr[b % 3], expected ? "MEME" : "DIFFERENT");
      TEST_ASSERT_EQUAL_MESSAGE(expected, got, msg);
      TEST_ASSERT_EQUAL_MESSAGE(got,
          historyImportSameSlot(ts[b / 3], gr[b % 3], ts[a / 3], gr[a % 3]),
          "La relation de creneau doit etre SYMETRIQUE.");
    }
  }
}

static void test_sameslot_bornes_uint32_sans_repliement(void) {
  // L'historique du projet a deja paye un repliement (millis() a 24,8 j).
  // Ici les timestamps sont des uint32 : 2038 et au-dela doivent se comparer
  // normalement, et 0xFFFFFFFF ne doit pas s'identifier a 0.
  TEST_ASSERT_TRUE(historyImportSameSlot(4294967295u, GRAN_HOURLY, 4294967295u, GRAN_HOURLY));
  TEST_ASSERT_FALSE_MESSAGE(historyImportSameSlot(4294967295u, GRAN_HOURLY, 0u, GRAN_HOURLY),
      "0xFFFFFFFF et 0 sont deux creneaux distincts : aucun repliement.");
  TEST_ASSERT_TRUE(historyImportSameSlot(2147483648u, GRAN_DAILY, 2147483648u, GRAN_DAILY));
}

// =============================================================================
// PARTIE 3 — historyImportTrimPlan : le bornage, soustractions GARDÉES
// =============================================================================

static void test_trim_sous_les_plafonds_ne_purge_rien(void) {
  const HistoryImportTrimPlan p = historyImportTrimPlan(10, 20, 5);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, p.rawToRemove, "Sous plafond RAW : rien a purger.");
  TEST_ASSERT_EQUAL_UINT(0, p.hourlyToRemove);
  TEST_ASSERT_EQUAL_UINT(0, p.dailyToRemove);
  TEST_ASSERT_TRUE_MESSAGE(p.empty(), "empty() doit refleter les trois compteurs.");
}

static void test_trim_historique_vide_aucun_underflow(void) {
  // LE cas qui detruit tout : `0 - 96` sur un size_t vaut ~1,8e19, donc
  // « purger 18 milliards de milliards de points » = purge TOTALE.
  const HistoryImportTrimPlan p = historyImportTrimPlan(0, 0, 0);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, p.rawToRemove,
      "Effectif 0 : la soustraction DOIT etre gardee. Non gardee, elle rend "
      "SIZE_MAX et la coquille purge l'historique ENTIER.");
  TEST_ASSERT_EQUAL_UINT(0, p.hourlyToRemove);
  TEST_ASSERT_EQUAL_UINT(0, p.dailyToRemove);
  TEST_ASSERT_TRUE(p.empty());
  // Balayage de tous les effectifs sous chaque plafond : aucun ne doit purger.
  for (size_t n = 0; n <= kMaxRawDataPoints; ++n) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, historyImportTrimPlan(n, 0, 0).rawToRemove,
        "Aucun effectif <= plafond ne doit produire de purge (garde d'underflow).");
  }
  for (size_t n = 0; n <= kMaxHourlyDataPoints; ++n) {
    TEST_ASSERT_EQUAL_UINT(0, historyImportTrimPlan(0, n, 0).hourlyToRemove);
  }
  for (size_t n = 0; n <= kMaxDailyDataPoints; ++n) {
    TEST_ASSERT_EQUAL_UINT(0, historyImportTrimPlan(0, 0, n).dailyToRemove);
  }
}

static void test_trim_exactement_au_plafond_ne_purge_rien(void) {
  // Borne inclusive : le plafond est le nombre de points AUTORISES, pas le
  // premier interdit. Un `>=` ici jetterait un point a chaque import d'un
  // historique plein — 339 points entrent, 336 restent, sans raison.
  const HistoryImportTrimPlan p = historyImportTrimPlan(
      kMaxRawDataPoints, kMaxHourlyDataPoints, kMaxDailyDataPoints);
  TEST_ASSERT_TRUE_MESSAGE(p.empty(),
      "96 RAW + 168 HOURLY + 75 DAILY = le PIRE CAS STOCKABLE (339). Il doit "
      "tenir INTACT : c'est le critere d'acceptation on-target de bug-101 "
      "(dropped = 0 sur history-339.csv).");
}

static void test_trim_un_seul_point_au_dela_du_plafond(void) {
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1,
      historyImportTrimPlan(kMaxRawDataPoints + 1, 0, 0).rawToRemove,
      "Un point de trop = un point a evincer, pas zero (off-by-one bas).");
  TEST_ASSERT_EQUAL_UINT(1, historyImportTrimPlan(0, kMaxHourlyDataPoints + 1, 0).hourlyToRemove);
  TEST_ASSERT_EQUAL_UINT(1, historyImportTrimPlan(0, 0, kMaxDailyDataPoints + 1).dailyToRemove);
}

static void test_trim_les_trois_granularites_sont_independantes(void) {
  // Un debordement RAW ne doit JAMAIS faire evincer des agregats : les DAILY
  // portent 75 jours d'historique, les RAW 8 heures.
  const HistoryImportTrimPlan p = historyImportTrimPlan(kMaxRawDataPoints + 40, 12, 3);
  TEST_ASSERT_EQUAL_UINT(40, p.rawToRemove);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, p.hourlyToRemove,
      "Un debordement RAW ne doit pas toucher aux HOURLY.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, p.dailyToRemove,
      "Un debordement RAW ne doit pas toucher aux DAILY (75 jours d'historique).");
  const HistoryImportTrimPlan q = historyImportTrimPlan(5, kMaxHourlyDataPoints + 32, 2);
  TEST_ASSERT_EQUAL_UINT(0, q.rawToRemove);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(32, q.hourlyToRemove,
      "history-overflow.csv : 200 HOURLY pour un plafond de 168 = 32 evinces, "
      "et ce 32 doit REMONTER au client (`dropped`), jamais rester muet.");
  TEST_ASSERT_EQUAL_UINT(0, q.dailyToRemove);
}

static void test_trim_le_plan_est_la_difference_exacte(void) {
  // Propriete generale, pas quelques points : effectif - plafond, ou 0.
  const size_t deltas[6] = {0, 1, 2, 17, 96, 500};
  for (size_t i = 0; i < 6; ++i) {
    const size_t d = deltas[i];
    const HistoryImportTrimPlan p = historyImportTrimPlan(
        kMaxRawDataPoints + d, kMaxHourlyDataPoints + d, kMaxDailyDataPoints + d);
    TEST_ASSERT_EQUAL_UINT(d, p.rawToRemove);
    TEST_ASSERT_EQUAL_UINT(d, p.hourlyToRemove);
    TEST_ASSERT_EQUAL_UINT(d, p.dailyToRemove);
    TEST_ASSERT_EQUAL_MESSAGE(d != 0, !p.empty(),
        "empty() doit etre le complement exact de « il y a quelque chose a evincer ».");
  }
}

static void test_trim_les_plafonds_viennent_de_constants_h(void) {
  // Source UNIQUE : si un plafond bouge dans constants.h, le plan suit sans
  // qu'on touche a la logique. On le prouve en s'appuyant sur les constantes
  // elles-memes, jamais sur 96/168/75 ecrits en dur ici.
  TEST_ASSERT_EQUAL_UINT(0, historyImportTrimPlan(kMaxRawDataPoints, 0, 0).rawToRemove);
  TEST_ASSERT_EQUAL_UINT(1, historyImportTrimPlan(kMaxRawDataPoints + 1, 0, 0).rawToRemove);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(339,
      kMaxRawDataPoints + kMaxHourlyDataPoints + kMaxDailyDataPoints,
      "Le total de 339 points chiffre l'allocation contigue de /get-history "
      "(339 x 28 = 9492 o) ET le pire cas de la fixture history-339.csv. Si ce "
      "total change, regenerer les fixtures (tools/gen_history_fixture.py) et "
      "recompter l'invariant memoire du critere on-target.");
}

// =============================================================================
// PARTIE 4 — BANC DE FUSION : l'IDEMPOTENCE EXACTE
// =============================================================================
// Le banc reproduit la COQUILLE d'`HistoryManager::appendData()` — fusion en
// place, tri, bornage — mais DÉLÈGUE toutes ses décisions aux trois fonctions
// pures testées ci-dessus. C'est ce qui le rend sensible aux mutations du
// module : muter `historyImportSameSlot` fait rougir l'idempotence.
//
// ⚠️ CE QU'IL NE PROUVE PAS : que la coquille réelle appelle bien ces fonctions,
// ni qu'elle écrase TOUTES les occurrences. Ces deux points-là sont couverts
// par les sentinelles structurelles de la partie 5, et par les DEUX variantes
// délibérément fautives ci-dessous, qui montrent que l'invariant TOMBE si la
// coquille dévie.

struct Pt {
  uint32_t ts;
  uint8_t gran;
  float ph;
  uint8_t mask;
  bool operator==(const Pt& o) const {
    return ts == o.ts && gran == o.gran && mask == o.mask &&
           ((isnan(ph) && isnan(o.ph)) || ph == o.ph);
  }
};

// `overwriteAll` : écraser toutes les occurrences d'un créneau (comportement
// livré) ou seulement la première (variante fautive).
// `evictOldest`  : évincer les plus anciens (livré) ou les plus récents (fautif).
static size_t benchAppend(std::vector<Pt>& buf, const std::vector<Pt>& in,
                          bool overwriteAll = true, bool evictOldest = true) {
  for (const Pt& p : in) {
    bool found = false;
    for (Pt& live : buf) {
      if (historyImportSameSlot(p.ts, p.gran, live.ts, live.gran)) {
        live = p;
        found = true;
        if (!overwriteAll) break;
      }
    }
    if (!found) buf.push_back(p);
  }
  std::stable_sort(buf.begin(), buf.end(),
                   [](const Pt& a, const Pt& b) { return a.ts < b.ts; });

  size_t raw = 0, hourly = 0, daily = 0;
  for (const Pt& p : buf) {
    if (p.gran == GRAN_RAW) raw++;
    else if (p.gran == GRAN_HOURLY) hourly++;
    else if (p.gran == GRAN_DAILY) daily++;
  }
  const HistoryImportTrimPlan plan = historyImportTrimPlan(raw, hourly, daily);
  if (plan.empty()) return 0;

  const size_t before = buf.size();
  if (!evictOldest) std::reverse(buf.begin(), buf.end());
  size_t dR = 0, dH = 0, dD = 0;
  buf.erase(std::remove_if(buf.begin(), buf.end(), [&](const Pt& p) {
              if (p.gran == GRAN_RAW && dR < plan.rawToRemove) { dR++; return true; }
              if (p.gran == GRAN_HOURLY && dH < plan.hourlyToRemove) { dH++; return true; }
              if (p.gran == GRAN_DAILY && dD < plan.dailyToRemove) { dD++; return true; }
              return false;
            }),
            buf.end());
  if (!evictOldest) std::reverse(buf.begin(), buf.end());
  return before - buf.size();
}

static size_t countDuplicateSlots(const std::vector<Pt>& buf) {
  size_t dup = 0;
  for (size_t i = 0; i < buf.size(); ++i) {
    for (size_t j = i + 1; j < buf.size(); ++j) {
      if (historyImportSameSlot(buf[i].ts, buf[i].gran, buf[j].ts, buf[j].gran)) ++dup;
    }
  }
  return dup;
}

// Jeu synthétique calqué sur test/fixtures/history-339.csv : les trois
// granularités exactement à leurs plafonds, sans chevauchement de créneau.
static std::vector<Pt> makeWorstCaseBatch() {
  std::vector<Pt> v;
  const uint32_t ref = 1757075400u;  // 2026-09-05 14:30 (reference des fixtures)
  const uint32_t rawStart = ref - 300u * (uint32_t)(kMaxRawDataPoints - 1);
  const uint32_t hourlyEnd = rawStart - 3600u;
  const uint32_t hourlyStart = hourlyEnd - 3600u * (uint32_t)(kMaxHourlyDataPoints - 1);
  const uint32_t dailyEnd = hourlyStart - 86400u;
  const uint32_t dailyStart = dailyEnd - 86400u * (uint32_t)(kMaxDailyDataPoints - 1);
  for (size_t i = 0; i < kMaxDailyDataPoints; ++i)
    v.push_back({dailyStart + 86400u * (uint32_t)i, GRAN_DAILY, 7.10f + 0.001f * (float)i, MASK_PH});
  for (size_t i = 0; i < kMaxHourlyDataPoints; ++i)
    v.push_back({hourlyStart + 3600u * (uint32_t)i, GRAN_HOURLY, 7.00f + 0.002f * (float)i, 0});
  for (size_t i = 0; i < kMaxRawDataPoints; ++i)
    v.push_back({rawStart + 300u * (uint32_t)i, GRAN_RAW, 6.95f + 0.003f * (float)i, MASK_UNKNOWN});
  return v;
}

static void test_fusion_pire_cas_339_entre_intact(void) {
  std::vector<Pt> buf;
  const std::vector<Pt> batch = makeWorstCaseBatch();
  TEST_ASSERT_EQUAL_UINT(339, batch.size());
  const size_t dropped = benchAppend(buf, batch);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, dropped,
      "history-339.csv est le pire cas STOCKABLE : importe dans un historique "
      "vide, il ne doit rien evincer. dropped != 0 ici veut dire que l'appareil "
      "ne peut pas restaurer sa propre sauvegarde maximale.");
  TEST_ASSERT_EQUAL_UINT(339, buf.size());
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, countDuplicateSlots(buf),
      "Aucun doublon de creneau apres import.");
}

static void test_fusion_idempotence_exacte_du_meme_lot(void) {
  // ⚠️ LE TEST CENTRAL DU LOT. C'est lui qui aurait attrape le defaut de
  // deduplication RAW : reenvoyer le meme lot doit laisser le MEME etat.
  // Sans quoi une reprise apres coupure reseau — ou un simple double clic —
  // double l'historique.
  const std::vector<Pt> batch = makeWorstCaseBatch();
  std::vector<Pt> une;
  benchAppend(une, batch);
  std::vector<Pt> deux;
  benchAppend(deux, batch);
  const size_t dropped2 = benchAppend(deux, batch);  // MEME lot, seconde fois

  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, dropped2,
      "Le reenvoi du MEME lot ne doit RIEN evincer. dropped != 0 signifie que "
      "les points ont ete recrees au lieu d'etre ecrases : des points anciens "
      "reels sont alors jetes pour faire place a des doublons.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(une.size(), deux.size(),
      "IDEMPOTENCE : deux envois du meme lot doivent donner le MEME effectif "
      "qu'un seul. C'est ce qui rend une reprise apres coupure sure.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, countDuplicateSlots(deux),
      "Aucun doublon de creneau apres le second envoi.");
  for (size_t i = 0; i < une.size(); ++i) {
    char msg[220];
    snprintf(msg, sizeof(msg),
        "Point %u : l'etat apres deux envois doit etre IDENTIQUE, valeur par "
        "valeur, a l'etat apres un seul (pas une moyenne, pas un doublon).",
        (unsigned)i);
    TEST_ASSERT_TRUE_MESSAGE(une[i] == deux[i], msg);
  }
}

static void test_fusion_idempotence_tient_sur_dix_reenvois(void) {
  // n envois == 1 envoi, pour n quelconque. Une derive de 1 point par envoi
  // (le defaut RAW) serait invisible sur 2 envois si le bornage la masquait ;
  // sur 10, elle ne l'est plus.
  const std::vector<Pt> batch = makeWorstCaseBatch();
  std::vector<Pt> ref;
  benchAppend(ref, batch);
  std::vector<Pt> buf;
  size_t droppedTotal = 0;
  for (int k = 0; k < 10; ++k) droppedTotal += benchAppend(buf, batch);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, droppedTotal,
      "Dix envois du meme lot : ZERO eviction cumulee.");
  TEST_ASSERT_EQUAL_UINT(ref.size(), buf.size());
  for (size_t i = 0; i < ref.size(); ++i) TEST_ASSERT_TRUE(ref[i] == buf[i]);
}

static void test_fusion_lot_raw_reenvoye_ne_double_pas(void) {
  // Le cas exact du defaut : 96 points RAW, deux fois. `dedupAggregatedPoints()`
  // ne les couvre PAS (history.cpp:1386, `if (p.granularity != RAW)`), donc
  // seule la regle de collision d'appendData les protege.
  std::vector<Pt> buf;
  std::vector<Pt> raws;
  for (size_t i = 0; i < kMaxRawDataPoints; ++i)
    raws.push_back({1757000000u + 300u * (uint32_t)i, GRAN_RAW, 7.0f, 0});
  benchAppend(buf, raws);
  TEST_ASSERT_EQUAL_UINT(kMaxRawDataPoints, buf.size());
  const size_t dropped = benchAppend(buf, raws);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(kMaxRawDataPoints, buf.size(),
      "96 RAW reenvoyes doivent rester 96, pas devenir 192 puis retomber a 96 "
      "en jetant les vrais anciens. C'est LE doublon muet de bug-101.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, dropped,
      "Zero eviction : rien de neuf n'est entre, donc rien ne doit sortir.");
  TEST_ASSERT_EQUAL_UINT(0, countDuplicateSlots(buf));
}

static void test_fusion_ecrase_bien_avec_la_valeur_entrante(void) {
  // L'ecrasement, pas la moyenne : `historyMergeMeanFinite` fabriquerait une
  // mesure qui n'a jamais existe, et la rendrait dependante du NOMBRE de
  // renvois. Sur une reprise, on veut la sauvegarde TELLE QUELLE.
  std::vector<Pt> buf = {{1000u, GRAN_HOURLY, 6.00f, 0}};
  const std::vector<Pt> in = {{1000u, GRAN_HOURLY, 7.40f, MASK_PH}};
  benchAppend(buf, in);
  TEST_ASSERT_EQUAL_UINT(1, buf.size());
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(7.40f, buf[0].ph,
      "La valeur ENTRANTE doit ecraser la vivante. 6,70 (la moyenne) serait une "
      "mesure inventee ; 6,00 serait la perte de l'import.");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(MASK_PH, buf[0].mask,
      "Le masque de dosage suit le point entrant, il n'est pas unionne.");
}

static void test_fusion_ecrase_TOUTES_les_occurrences_d_un_creneau(void) {
  // Un fichier ecrit par une version <= 2.18.0 peut porter des DOUBLONS
  // d'agregats. N'ecraser que le premier laisserait l'autre PERIME, que
  // dedupAggregatedPoints() moyennerait ensuite avec le neuf : l'idempotence
  // serait perdue et une valeur fausse apparaitrait.
  std::vector<Pt> buf = {
      {1000u, GRAN_HOURLY, 6.00f, 0},
      {1000u, GRAN_HOURLY, 6.20f, 0},  // doublon herite
      {2000u, GRAN_HOURLY, 7.00f, 0},
  };
  const std::vector<Pt> in = {{1000u, GRAN_HOURLY, 7.40f, MASK_PH}};
  benchAppend(buf, in);
  size_t stale = 0;
  for (const Pt& p : buf) if (p.ts == 1000u && p.ph != 7.40f) ++stale;
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, stale,
      "AUCUNE occurrence perimee du creneau 1000/HOURLY ne doit subsister. En "
      "n'ecrasant que la PREMIERE, il en reste une a 6,20 que la consolidation "
      "moyennera ensuite avec 7,40 -> 6,80, une valeur jamais mesuree.");
}

static void test_fusion_variante_fautive_premiere_occurrence_perd_l_invariant(void) {
  // CONTRE-EPREUVE : elle prouve que le test precedent MORD. Sans elle, on ne
  // saurait pas si l'assertion ci-dessus est satisfaite par la regle ou par
  // hasard (par exemple si le jeu ne contenait pas vraiment de doublon).
  std::vector<Pt> buf = {
      {1000u, GRAN_HOURLY, 6.00f, 0},
      {1000u, GRAN_HOURLY, 6.20f, 0},
      {2000u, GRAN_HOURLY, 7.00f, 0},
  };
  const std::vector<Pt> in = {{1000u, GRAN_HOURLY, 7.40f, MASK_PH}};
  benchAppend(buf, in, /*overwriteAll=*/false);
  size_t stale = 0;
  for (const Pt& p : buf) if (p.ts == 1000u && p.ph != 7.40f) ++stale;
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, stale,
      "Contre-epreuve : n'ecraser que la premiere occurrence LAISSE bien un "
      "point perime. Si ce compte tombe a 0, le jeu de test ne contient plus de "
      "doublon herite et l'assertion « toutes les occurrences » ne prouve plus "
      "rien — reparer le JEU, pas cette assertion.");
}

static void test_fusion_eviction_garde_les_points_les_plus_RECENTS(void) {
  // history-overflow.csv : 200 HOURLY pour un plafond de 168. Les 32 evinces
  // doivent etre les PLUS ANCIENS. Purger les recents rendrait un historique
  // qui s'arrete dans le passe, sans le dire — et le graphe paraitrait normal.
  std::vector<Pt> buf;
  std::vector<Pt> in;
  const size_t n = 200;
  for (size_t i = 0; i < n; ++i)
    in.push_back({1700000000u + 3600u * (uint32_t)i, GRAN_HOURLY, 7.0f, 0});
  const size_t dropped = benchAppend(buf, in);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(n - kMaxHourlyDataPoints, dropped,
      "200 HOURLY - plafond 168 = 32 evinces, et ce nombre doit REMONTER au "
      "client (`dropped`). Une perte muette est le defaut d'origine de bug-101.");
  TEST_ASSERT_EQUAL_UINT(kMaxHourlyDataPoints, buf.size());
  const uint32_t expectedOldest = 1700000000u + 3600u * (uint32_t)(n - kMaxHourlyDataPoints);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(expectedOldest, buf.front().ts,
      "Le plus ancien SURVIVANT doit etre le 33e point envoye : ce sont les 32 "
      "PREMIERS (les plus anciens) qui partent.");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1700000000u + 3600u * (uint32_t)(n - 1), buf.back().ts,
      "Le point le PLUS RECENT doit survivre. S'il manque, l'eviction purge "
      "par le mauvais bout et l'historique s'arrete dans le passe.");
}

static void test_fusion_variante_fautive_eviction_des_recents_est_detectee(void) {
  // CONTRE-EPREUVE de l'orientation de l'eviction (mutant n°3 du lot).
  std::vector<Pt> buf;
  std::vector<Pt> in;
  for (size_t i = 0; i < 200; ++i)
    in.push_back({1700000000u + 3600u * (uint32_t)i, GRAN_HOURLY, 7.0f, 0});
  benchAppend(buf, in, /*overwriteAll=*/true, /*evictOldest=*/false);
  TEST_ASSERT_EQUAL_UINT(kMaxHourlyDataPoints, buf.size());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1700000000u, buf.front().ts,
      "Contre-epreuve : purger par le mauvais bout conserve bien le point le "
      "plus ANCIEN et jette les recents. L'assertion precedente distingue donc "
      "reellement les deux orientations.");
}

static void test_fusion_lot_recent_dans_un_historique_plein(void) {
  // Cas d'usage reel du mode « completer » : ajouter quelques points recents a
  // un historique deja plein. `dropped` compte alors des points DEJA PRESENTS,
  // pas les entrants — c'est la definition que porte la reponse de la route et
  // le message de l'interface.
  std::vector<Pt> buf;
  benchAppend(buf, makeWorstCaseBatch());
  TEST_ASSERT_EQUAL_UINT(339, buf.size());
  const uint32_t last = buf.back().ts;
  std::vector<Pt> in;
  for (int i = 1; i <= 20; ++i)
    in.push_back({last + 300u * (uint32_t)i, GRAN_RAW, 7.2f, 0});
  const size_t dropped = benchAppend(buf, in);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(20, dropped,
      "20 RAW recents dans un historique plein : 20 evictions. Ce sont 20 "
      "points ANCIENS DEJA PRESENTS qui partent, jamais les entrants — la "
      "regle de collision etant l'ecrasement, un entrant n'est jamais refuse.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(339, buf.size(),
      "L'effectif reste au plafond total : rien n'a fui au-dessus.");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(last + 300u * 20u, buf.back().ts,
      "Les points entrants (les plus recents) sont bien ceux qui restent.");
  TEST_ASSERT_EQUAL_UINT(0, countDuplicateSlots(buf));
}

static void test_fusion_lot_portant_deux_fois_le_meme_creneau(void) {
  // Un CSV maltraite peut porter deux fois la meme ligne. La derniere doit
  // gagner, et il ne doit rester qu'un point — le point cree par la premiere
  // occurrence doit etre visible pour la seconde du MEME lot.
  std::vector<Pt> buf;
  const std::vector<Pt> in = {
      {1000u, GRAN_RAW, 6.9f, 0},
      {1000u, GRAN_RAW, 7.3f, MASK_PH},
  };
  benchAppend(buf, in);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, buf.size(),
      "Deux lignes du MEME creneau dans UN lot ne doivent produire qu'UN point.");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(7.3f, buf[0].ph, "La derniere occurrence gagne.");
}

static void test_fusion_lots_successifs_equivalent_a_un_lot_unique(void) {
  // Le decoupage client ne doit RIEN changer au resultat : 339 points envoyes
  // en 8 lots == 339 points envoyes d'un coup. C'est la condition pour que la
  // reprise apres coupure soit sans effet de bord.
  const std::vector<Pt> batch = makeWorstCaseBatch();
  std::vector<Pt> enUn;
  benchAppend(enUn, batch);

  std::vector<Pt> enLots;
  size_t droppedTotal = 0;
  const size_t chunk = 45;  // ~ ce que le budget d'octets produit
  for (size_t i = 0; i < batch.size(); i += chunk) {
    std::vector<Pt> part(batch.begin() + (long)i,
                         batch.begin() + (long)std::min(i + chunk, batch.size()));
    droppedTotal += benchAppend(enLots, part);
  }
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, droppedTotal,
      "Un decoupage en lots ne doit provoquer AUCUNE eviction que l'envoi "
      "unique ne provoquerait pas.");
  TEST_ASSERT_EQUAL_UINT(enUn.size(), enLots.size());
  for (size_t i = 0; i < enUn.size(); ++i) {
    char msg[200];
    snprintf(msg, sizeof(msg), "Point %u : decoupage != resultat.", (unsigned)i);
    TEST_ASSERT_TRUE_MESSAGE(enUn[i] == enLots[i], msg);
  }
}

static void test_fusion_reprise_apres_coupure_ne_cree_pas_de_doublon(void) {
  // Scenario du critere on-target : coupure Wi-Fi au 3e lot, puis reprise. Les
  // lots deja passes sont REJOUES (le client reprend a son index, mais un lot
  // peut avoir abouti sans que la reponse revienne). L'etat final doit etre le
  // meme que sans coupure — c'est tout l'objet de l'ecrasement.
  const std::vector<Pt> batch = makeWorstCaseBatch();
  const size_t chunk = 45;
  std::vector<std::vector<Pt>> lots;
  for (size_t i = 0; i < batch.size(); i += chunk)
    lots.push_back(std::vector<Pt>(batch.begin() + (long)i,
                                   batch.begin() + (long)std::min(i + chunk, batch.size())));
  std::vector<Pt> sansCoupure;
  benchAppend(sansCoupure, batch);

  std::vector<Pt> buf;
  size_t droppedTotal = 0;
  for (size_t i = 0; i < 3; ++i) droppedTotal += benchAppend(buf, lots[i]);
  const size_t partiel = buf.size();
  TEST_ASSERT_TRUE_MESSAGE(partiel > 0 && partiel < batch.size(),
      "Apres coupure au 3e lot, l'historique doit etre PARTIEL — jamais vide, "
      "jamais complet.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, countDuplicateSlots(buf),
      "L'etat partiel doit deja etre COHERENT (aucun doublon).");
  // Reprise : on rejoue le lot 3 (dont on ignore s'il a abouti) puis la suite.
  for (size_t i = 2; i < lots.size(); ++i) droppedTotal += benchAppend(buf, lots[i]);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, droppedTotal,
      "Une reprise qui rejoue un lot ne doit provoquer AUCUNE eviction.");
  TEST_ASSERT_EQUAL_UINT(sansCoupure.size(), buf.size());
  for (size_t i = 0; i < sansCoupure.size(); ++i) TEST_ASSERT_TRUE(sansCoupure[i] == buf[i]);
  TEST_ASSERT_EQUAL_UINT(0, countDuplicateSlots(buf));
}

// =============================================================================
// PARTIE 5 — CÂBLAGE DE LA COQUILLE (src/history.cpp, src/history.h)
// =============================================================================

static void test_sentinelle_appendData_delegue_la_regle_de_collision(void) {
  char* body = codeOfFunction("src/history.cpp", "bool HistoryManager::appendData(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "historyImportSameSlot(") >= 1,
      "appendData() doit APPELER historyImportSameSlot(). Reimplementer la "
      "comparaison sur place rendrait toute la partie 2 de cette suite "
      "decorative : elle testerait une fonction que plus personne n'appelle.");
  free(body);
}

static void test_sentinelle_appendData_n_exclut_pas_RAW(void) {
  char* body = codeOfFunction("src/history.cpp", "bool HistoryManager::appendData(");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "!= RAW"),
      "appendData() ne doit contenir AUCUN filtre `!= RAW`. C'est le filtre de "
      "dedupAggregatedPoints() (history.cpp:1386), et le RECOPIER ici "
      "reintroduirait les doublons RAW muets que ce lot corrige.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "== RAW"),
      "Aucun aiguillage sur RAW non plus : la fusion traite les trois "
      "granularites de la meme facon.");
  free(body);
}

static void test_sentinelle_appendData_ecrase_toutes_les_occurrences(void) {
  char* body = codeOfFunction("src/history.cpp", "bool HistoryManager::appendData(");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "break"),
      "AUCUN `break` dans appendData(). La boucle interne doit parcourir "
      "memoryBuffer EN ENTIER pour ecraser TOUTES les occurrences d'un creneau : "
      "un `break` apres la premiere laisserait un doublon herite (fichier "
      "<= 2.18.0) perime, que la consolidation moyennerait ensuite avec le neuf. "
      "Cf. test_fusion_ecrase_TOUTES_les_occurrences_d_un_creneau.");
  free(body);
}

static void test_sentinelle_appendData_borne_et_ne_moyenne_pas(void) {
  char* body = codeOfFunction("src/history.cpp", "bool HistoryManager::appendData(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "trimToCaps()") >= 1,
      "appendData() doit appeler trimToCaps(). Sans lui, le mode « completer » "
      "franchit les plafonds sans passer par aucun des deux bornages existants "
      "(recordDataPoint pour RAW, pas 4 de consolidateData pour les agregats).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "std::sort(") >= 1,
      "Le buffer doit etre TRIE avant trimToCaps() : c'est l'ordre du vecteur "
      "qui fait que « les premiers rencontres » = « les plus anciens ».");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "dedupAggregatedPoints("),
      "appendData() ne doit PAS appeler dedupAggregatedPoints() : il MOYENNE "
      "(historyMergeMeanFinite), ce qui re-moyennerait le point restaure avec le "
      "vivant qu'il devait remplacer et detruirait l'idempotence.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "historyMergeMeanFinite("),
      "Aucune moyenne dans appendData() : la regle est l'ECRASEMENT. Une moyenne "
      "fabriquerait une mesure jamais prise, et dependante du nombre de renvois.");
  free(body);
}

static void test_sentinelle_appendData_rapporte_total_et_dropped(void) {
  char* body = codeOfFunction("src/history.cpp", "bool HistoryManager::appendData(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "*outDropped") >= 1,
      "appendData() doit ecrire *outDropped. Une perte MUETTE est le defaut "
      "d'origine de bug-101 : l'utilisateur croyait avoir restaure sa sauvegarde.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "*outTotal") >= 1,
      "appendData() doit ecrire *outTotal (points detenus apres le lot).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "xSemaphoreGive") >= 1,
      "Le mutex doit etre RENDU. Un chemin de sortie qui l'oublie gele "
      "l'historique jusqu'au redemarrage.");
  free(body);
}

static void test_sentinelle_trimToCaps_delegue_le_plan_et_purge_les_anciens(void) {
  char* body = codeOfFunction("src/history.cpp", "void HistoryManager::trimToCaps(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "historyImportTrimPlan(") >= 1,
      "trimToCaps() doit APPELER historyImportTrimPlan() : c'est la seule "
      "soustraction gardee du chemin. Recalculer sur place ramene le risque "
      "d'underflow size_t, donc de purge totale.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "rbegin"),
      "trimToCaps() doit parcourir le buffer du DEBUT (les plus anciens). Un "
      "parcours inverse (rbegin/rend) purgerait les points les plus RECENTS et "
      "l'historique s'arreterait dans le passe, sans le dire.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "reverse"),
      "Aucune inversion du buffer avant purge, meme raison que ci-dessus.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "memoryBuffer.begin()") >= 1,
      "La purge part de memoryBuffer.begin() : l'ordre du vecteur EST l'ordre "
      "chronologique.");
  free(body);
}

static void test_sentinelle_non_regression_recordDataPoint_intact(void) {
  // NON-REGRESSION : bug-101 ajoute un TROISIEME site de bornage, il ne
  // remplace pas les deux existants. recordDataPoint() doit garder le sien.
  char* body = codeOfFunction("src/history.cpp", "void HistoryManager::recordDataPoint(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "MAX_RAW_POINTS") >= 1,
      "recordDataPoint() doit CONSERVER son bornage RAW propre. Le deleguer a "
      "trimToCaps() changerait le chemin chaud de la boucle (appele toutes les "
      "5 min) : hors perimetre de ce lot.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "trimToCaps("),
      "recordDataPoint() ne doit PAS appeler trimToCaps() : trimToCaps est "
      "reserve a appendData (cf. history.h), et l'y brancher modifierait le "
      "comportement du chemin d'enregistrement normal.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "appendData("),
      "recordDataPoint() ne doit pas passer par appendData().");
  free(body);
}

static void test_sentinelle_non_regression_consolidateData_intact(void) {
  char* body = codeOfFunction("src/history.cpp", "void HistoryManager::consolidateData(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "MAX_HOURLY_POINTS") >= 1,
      "Le pas 4 de consolidateData() doit CONSERVER son bornage HOURLY.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "MAX_DAILY_POINTS") >= 1,
      "Le pas 4 de consolidateData() doit CONSERVER son bornage DAILY.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "dedupAggregatedPoints()") >= 1,
      "consolidateData() doit CONTINUER a dedoublonner ses agregats : c'est "
      "l'autre chemin, et bug-101 ne le touche pas.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "trimToCaps("),
      "consolidateData() ne doit PAS appeler trimToCaps() (perimetre : "
      "appendData seul, cf. history.h).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "historyImportSameSlot("),
      "consolidateData() ne doit pas emprunter la regle de collision de "
      "l'import : elle ECRASE, alors que la consolidation MOYENNE.");
  free(body);
}

static void test_sentinelle_sizeof_datapoint_reste_verrouille_a_28(void) {
  char* src = readProjectCode("src/history.h");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(src, "static_assert(sizeof(DataPoint) == 28") >= 1,
      "Le verrou sizeof(DataPoint) == 28 doit rester dans src/history.h. C'est "
      "lui qui chiffre l'allocation contigue de /get-history (339 x 28 = 9492 o) "
      "et l'invariant memoire du critere on-target de bug-101 : « l'interne ne "
      "bouge pas de plus de 9492 o ». Un champ de plus deplace ce chiffre.");
  free(src);
}

static void test_sentinelle_enum_granularite_inchange(void) {
  // Les valeurs RAW=0 / HOURLY=1 / DAILY=2 sont recopiees en tete de ce fichier
  // ET ecrites dans les fixtures CSV (colonne `granularity`). Si l'enum est
  // reordonne, les fixtures designent d'autres granularites en silence.
  char* src = readProjectCode("src/history.h");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_TRUE_MESSAGE(strstr(src, "RAW") != NULL && strstr(src, "HOURLY") != NULL &&
                           strstr(src, "DAILY") != NULL,
      "Les trois granularites doivent exister dans src/history.h.");
  const char* e = strstr(src, "enum Granularity");
  TEST_ASSERT_NOT_NULL_MESSAGE(e, "`enum Granularity` introuvable dans src/history.h.");
  const char* pRaw = strstr(e, "RAW");
  const char* pHourly = strstr(e, "HOURLY");
  const char* pDaily = strstr(e, "DAILY");
  TEST_ASSERT_NOT_NULL(pRaw); TEST_ASSERT_NOT_NULL(pHourly); TEST_ASSERT_NOT_NULL(pDaily);
  TEST_ASSERT_TRUE_MESSAGE(pRaw < pHourly && pHourly < pDaily,
      "Ordre de l'enum : RAW(0) < HOURLY(1) < DAILY(2). Le reordonner sans "
      "regenerer test/fixtures/*.csv ferait designer d'AUTRES granularites par "
      "la colonne `granularity` des fixtures — silencieusement.");
  free(src);
}

// =============================================================================
// PARTIE 6 — bug-113 : LE GARDE DE CORPS JSON ET SON CÂBLAGE
// =============================================================================

static void test_sentinelle_garde_compare_strictement_le_plafond(void) {
  // ⚠️ OFF-BY-ONE D'UN CORPS DE 8 192 OCTETS EXACTEMENT.
  // La bibliotheque refuse sur `contentLength() > _maxContentLength`
  // (AsyncJson.cpp:158 et :203) : le plafond est le nombre d'octets AUTORISES.
  // Un `>=` dans le garde refuserait un corps que la bibliotheque, elle,
  // accepterait — les deux plafonds cesseraient de designer la meme frontiere,
  // sur exactement un octet. Le budget client (7 168) laisse 1 024 o de marge,
  // donc le cas ne se produirait qu'avec un client tiers ou une page en cache :
  // introuvable en recette, et un 413 inexplicable en production.
  char* body = codeOfFunction("src/web_helpers.cpp", "void registerJsonBodyGuard(");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, ">= capBytes"),
      "AUCUN `>= capBytes` dans registerJsonBodyGuard : un corps de "
      "EXACTEMENT kHistoryImportMaxBodyBytes octets doit etre ACCEPTE, comme le "
      "fait AsyncJson.cpp:158/203. Sinon le garde et la bibliotheque divergent "
      "d'un octet.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(body, "contentLength() > capBytes"),
      "DEUX comparaisons STRICTES attendues : une dans le filtre (verdict) et "
      "une dans le handler (choix 413 vs 503). Elles doivent rester "
      "IDENTIQUES : si le filtre accepte et que le handler juge autrement, la "
      "requete recoit un 503 pour un probleme de taille.");
  free(body);
}

static void test_sentinelle_garde_rend_413_et_503_actionnables(void) {
  char* body = codeOfFunction("src/web_helpers.cpp", "void registerJsonBodyGuard(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "413") >= 1,
      "Le garde doit rendre un 413 pour un corps hors gabarit.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "503") >= 1,
      "Le garde doit rendre un 503 en famine memoire.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "Retry-After") >= 1,
      "Le 503 doit porter Retry-After, sinon le client reessaie en boucle.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "httpLowHeapShouldRefuse(") >= 1,
      "Le verdict de famine doit passer par le predicat pur EXISTANT "
      "httpLowHeapShouldRefuse (deja couvert par test_native_http_lowheap_guard) "
      "et non par un seuil invente ici.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "heapLargestFreeBlock8Bit()") >= 1,
      "La mesure doit etre celle du tas 8 BITS. ESP.getFreeHeap() compte "
      "l'IRAM 32 bits : c'est la metrique FAUSSE du projet (cause racine OOM du "
      "15/08/2026), et elle rendrait la garde aveugle a la vraie famine.");
  free(body);
}

static void test_sentinelle_garde_n_alloue_ni_ne_journalise_sur_async_tcp(void) {
  char* body = codeOfFunction("src/web_helpers.cpp", "void registerJsonBodyGuard(");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "systemLogger"),
      "AUCUN log dans le garde : il tourne sur la tache `async_tcp`, et un "
      "WARNING ou pire declenche un flush SYNCHRONE du journal, donc un fopen — "
      "exactement l'abort() que ce garde est cense prevenir.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "String("),
      "Aucune String construite dans le garde : le message part par snprintf "
      "dans un tampon de PILE. Concatener une String allouerait sur async_tcp, "
      "en pleine famine.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "snprintf(") >= 1,
      "Le message 413 doit etre formate par snprintf dans un tampon de pile.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "request->url()") >= 1,
      "Le filtre doit comparer l'URI sans allouer (pas de String path = ...).");
  free(body);
}

// Vérifie qu'une route est gardée AVANT l'enregistrement de son handler JSON.
// L'ORDRE est la substance de la garde : AsyncWebServer::_attachHandler retient
// le PREMIER handler dont filter() et canHandle() passent. Enregistré après,
// le garde ne voit jamais rien — et le test qui se contenterait de constater sa
// PRÉSENCE serait vert sur un garde totalement inerte.
static void assertGuardRegisteredBeforeHandler(const char* file, const char* uri,
                                               const char* capName,
                                               const char* addHandlerNeedle) {
  char* src = readProjectCode(file);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "Fichier de routes introuvable.");
  char needle[256];
  snprintf(needle, sizeof(needle), "registerJsonBodyGuard(server, \"%s\", %s)", uri, capName);
  const char* guard = strstr(src, needle);
  char msg[520];
  snprintf(msg, sizeof(msg),
      "%s doit appeler exactement `%s`. Sans cet appel, la route tamponne "
      "jusqu'a 16 384 o (defaut de AsyncCallbackJsonWebHandler) AVANT toute "
      "authentification — c'est bug-113.", file, needle);
  TEST_ASSERT_NOT_NULL_MESSAGE(guard, msg);
  const char* add = strstr(src, addHandlerNeedle);
  snprintf(msg, sizeof(msg), "`%s` introuvable dans %s.", addHandlerNeedle, file);
  TEST_ASSERT_NOT_NULL_MESSAGE(add, msg);
  snprintf(msg, sizeof(msg),
      "ORDRE INVERSE dans %s : registerJsonBodyGuard(\"%s\") doit venir AVANT "
      "`%s`. AsyncWebServer retient le PREMIER handler dont filter() et "
      "canHandle() passent (WebServer.cpp:147) : enregistre apres, le garde ne "
      "voit JAMAIS passer une requete et le plafond n'existe pas.",
      file, uri, addHandlerNeedle);
  TEST_ASSERT_TRUE_MESSAGE(guard < add, msg);
  free(src);
}

static void test_sentinelle_route_import_gardee_avant_son_handler(void) {
  assertGuardRegisteredBeforeHandler("src/web_routes_data.cpp", "/history/import",
                                     "kHistoryImportMaxBodyBytes",
                                     "server->addHandler(importHandler)");
  // Le plafond doit AUSSI etre pose sur le handler de la bibliotheque : le
  // garde couvre le Content-Length annonce, setMaxContentLength couvre le total
  // reellement recu (un client peut mentir sur son en-tete).
  char* src = readProjectCode("src/web_routes_data.cpp");
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(src, "setMaxContentLength(kHistoryImportMaxBodyBytes)") >= 1,
      "importHandler->setMaxContentLength(kHistoryImportMaxBodyBytes) doit etre "
      "pose : le garde lit l'en-tete Content-Length (annonce), la bibliotheque "
      "compte les octets REELLEMENT recus. Un client qui sous-declare son corps "
      "passerait le garde ; seul setMaxContentLength l'arrete.");
  free(src);
}

static void test_sentinelle_routes_wifi_et_capteur_gardees_avant_leur_handler(void) {
  // bug-113 concerne TROIS routes, pas une : 3 x 16 Ko reclamables sans
  // identifiants. En garder deux ne divise le total que d'un tiers.
  assertGuardRegisteredBeforeHandler("src/web_routes_config.cpp", "/wifi/connect",
                                     "kWifiConnectMaxBodyBytes",
                                     "addHandler(");
  assertGuardRegisteredBeforeHandler("src/web_routes_sensor_id.cpp",
                                     "/sensors/onewire/identify",
                                     "kSensorIdentifyMaxBodyBytes",
                                     "addHandler(");
}

static void test_sentinelle_route_import_aiguille_par_la_regle_de_mode(void) {
  char* src = readProjectCode("src/web_routes_data.cpp");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, "historyImportModeIsReplace(") >= 1,
      "La route doit APPELER historyImportModeIsReplace(). Lire `mode` en dur "
      "(strcmp sur place) rendrait toute la partie 1 de cette suite decorative.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, "history.importData(imported)") >= 1,
      "La branche `replace` doit rester importData() : c'est le comportement "
      "historique, celui que tout client anterieur a v2.48.0 attend.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, "history.appendData(imported") >= 1,
      "La branche `append` doit appeler appendData().");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, "doc[\"dropped\"]") >= 1,
      "La reponse doit porter `dropped`, TOUJOURS — meme a zero. Une perte "
      "muette est le defaut d'origine de bug-101.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, "doc[\"total\"]") >= 1,
      "La reponse doit porter `total` (points detenus apres le lot) : c'est ce "
      "que le client affiche comme « conserve(s) sur l'appareil ».");
  free(src);
}

static void test_sentinelle_plafonds_de_corps_restent_sous_le_defaut_bibliotheque(void) {
  // Un plafond >= 16 384 ne surcharge PLUS RIEN (defaut de
  // AsyncCallbackJsonWebHandler) : la garde serait purement decorative, comme
  // l'a ete le parachute anti-OOM loge en PSRAM depuis feature-066.
  char* src = readProjectCode("src/constants.h");
  TEST_ASSERT_NOT_NULL(src);
  const long import = numberAfter(src, "kHistoryImportMaxBodyBytes");
  const long wifi = numberAfter(src, "kWifiConnectMaxBodyBytes");
  const long ident = numberAfter(src, "kSensorIdentifyMaxBodyBytes");
  TEST_ASSERT_TRUE_MESSAGE(import > 0 && wifi > 0 && ident > 0,
      "Les trois plafonds de corps doivent etre definis dans src/constants.h.");
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)kHistoryImportMaxBodyBytes, (int)import,
      "Le plafond lu dans le SOURCE doit egaler celui compile : sinon la "
      "sentinelle de parite ci-dessous compare des chiffres sans rapport.");
  TEST_ASSERT_TRUE_MESSAGE(import < 16384 && wifi < 16384 && ident < 16384,
      "Chaque plafond doit rester STRICTEMENT sous 16 384 : a 16 384 il ne "
      "surcharge plus le defaut de la bibliotheque et la garde devient "
      "decorative.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(9472, (int)(import + wifi + ident),
      "Total expose sans authentification : le chiffre -80,7 pourcent de la doc "
      "vaut 9 472 o (contre 3 x 16 384 = 49 152). Reviser un plafond oblige a "
      "RECOMPTER ce total, et la doc avec.");
  free(src);
}

// =============================================================================
// PARTIE 7 — SENTINELLE DE PARITÉ : budget client (data/app.js) ↔ plafond route
// =============================================================================
// Idiome de test_native_ws_data_parity : data/app.js ne compile pas en natif,
// et c'est le SEUL moyen de tester du JS ici.
//
// L'INVARIANT : budget client < plafond de route, STRICTEMENT. À égalité,
// l'enveloppe JSON (`{"mode":"replace","history":[…]}`, 31 o) suffirait à faire
// passer un lot nominal au-dessus du plafond → 413 sur une restauration
// normale, alors que rien n'est anormal.

static long jsChunkBudget(void) {
  char* src = readProjectCode("data/app.js");
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "data/app.js introuvable : la sentinelle de "
      "parite serait AVEUGLE.");
  const long v = numberAfter(src, "kHistoryImportChunkBudgetBytes =");
  free(src);
  TEST_ASSERT_TRUE_MESSAGE(v > 0,
      "`const kHistoryImportChunkBudgetBytes = <n>;` introuvable dans "
      "data/app.js (code DEPOUILLE de ses commentaires). Si le budget a ete "
      "renomme ou calcule, adapter cette sentinelle — ne pas la supprimer.");
  return v;
}

static void test_parite_budget_client_strictement_sous_le_plafond_de_route(void) {
  const long budget = jsChunkBudget();
  char msg[520];
  snprintf(msg, sizeof(msg),
      "BUDGET CLIENT HORS GABARIT : data/app.js decoupe a %ld o alors que "
      "POST /history/import plafonne a %u o (kHistoryImportMaxBodyBytes). Chaque "
      "lot nominal serait refuse en 413 et AUCUNE restauration ne passerait. Le "
      "budget doit rester STRICTEMENT inferieur au plafond — a egalite, "
      "l'enveloppe JSON (31 o) suffit a le franchir.",
      budget, (unsigned)kHistoryImportMaxBodyBytes);
  TEST_ASSERT_TRUE_MESSAGE(budget < (long)kHistoryImportMaxBodyBytes, msg);
}

static void test_parite_budget_client_egale_la_constante_firmware(void) {
  // Le firmware porte la MEME valeur (kHistoryImportChunkBudgetBytes) pour
  // pouvoir chiffrer sa marge par static_assert. Les deux doivent coincider,
  // sinon le static_assert de constants.h certifie une marge que le client
  // n'applique pas.
  const long budget = jsChunkBudget();
  char msg[480];
  snprintf(msg, sizeof(msg),
      "DIVERGENCE : data/app.js decoupe a %ld o, le firmware documente %u o "
      "(kHistoryImportChunkBudgetBytes). Le static_assert de constants.h "
      "certifie alors une marge de 1 024 o que le client n'applique pas.",
      budget, (unsigned)kHistoryImportChunkBudgetBytes);
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)kHistoryImportChunkBudgetBytes, (int)budget, msg);
}

// ---------------------------------------------------------------------------
// La propriete qui a sauve l'invariant : MESURER, jamais diviser.
// ---------------------------------------------------------------------------
// QUATRE chiffres de « poids d'un point » se sont succede dans ce projet, tous
// faux : 211-213 o (supposait les colonnes filtrees), 149 o (un cas particulier,
// l'ORP negatif), 157 o (emprunte au pire cas de l'EXPORT firmware et presente
// comme une borne conservatrice, alors que le maximum theorique de 172 o le
// depasse), et 148 o lui-meme n'est que le pire cas d'UNE fixture.
//
// Aucun n'a jamais casse le decoupage, et ce n'est pas de la chance :
// `chunkHistoryPoints()` MESURE les octets de chaque point
// (jsonByteLength(JSON.stringify(point))) au lieu de diviser le budget par un
// poids suppose. Remplacer cette mesure par une division ferait de n'importe
// quel chiffre errone un 413 en pleine restauration.
//
// Cette sentinelle verrouille la METHODE, pas le chiffre — c'est exactement ce
// que prescrit l'ADR-0073.

static void test_sentinelle_le_decoupage_mesure_les_octets_et_ne_divise_pas(void) {
  char* src = readProjectCode("data/app.js");
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "data/app.js introuvable.");
  char* body = extractFunctionBody(src, "function chunkHistoryPoints(");
  free(src);
  TEST_ASSERT_NOT_NULL_MESSAGE(body,
      "`function chunkHistoryPoints(` introuvable dans data/app.js DEPOUILLE de "
      "ses commentaires. Si le decoupage a ete renomme, adapter cette "
      "sentinelle — ne pas la supprimer.");

  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "jsonByteLength(JSON.stringify(point))") >= 1,
      "REGRESSION DE METHODE : chunkHistoryPoints() doit mesurer les octets de "
      "CHAQUE point via jsonByteLength(JSON.stringify(point)). Sans cette "
      "mesure, le decoupage repose sur un poids suppose — et QUATRE poids "
      "successifs se sont deja reveles faux dans ce projet (211-213, 149, 157, "
      "et 148 qui n'est que le pire cas d'une fixture). Le maximum THEORIQUE "
      "est 172 o : une division par un chiffre plus petit produit un 413 en "
      "pleine restauration. Voir ADR-0073.");

  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "jsonByteLength(kHistoryImportEnvelope)") >= 1,
      "L'enveloppe {\"mode\":…,\"history\":[]} doit etre comptee elle aussi, et "
      "MESUREE de la meme facon : c'est elle qui ferait franchir le plafond a "
      "un lot calcule au ras du budget.");

  // Une division du budget par un poids par point : le defaut exact que la
  // mesure evite. `budgetBytes /` ou `budget /` n'a aucune raison d'exister.
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "budgetBytes /"),
      "chunkHistoryPoints() DIVISE le budget : c'est le retour du defaut que la "
      "mesure d'octets evitait. Le nombre de points par lot ne doit etre ecrit "
      "NULLE PART — il se deduit de la mesure, point par point.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "budgetBytes/"),
      "Idem, sans espace : chunkHistoryPoints() ne divise pas le budget.");
  free(body);
}

static void test_parite_marge_de_1024_octets_entre_budget_et_plafond(void) {
  const long budget = jsChunkBudget();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1024,
      (int)((long)kHistoryImportMaxBodyBytes - budget),
      "La marge budget -> plafond doit valoir 1 024 o : elle absorbe "
      "l'enveloppe, l'echappement d'un caractere inattendu et la variabilite de "
      "longueur des nombres, sans jamais frôler le 413.");
}

static void test_parite_budget_client_reste_au_dessus_du_seuil_psram(void) {
  // ⚠️ COROLLAIRE CONTRE-INTUITIF de bug-113, etabli par DESASSEMBLAGE de
  // libheap.a le 07/09/2026 : heap_caps_malloc_default n'oriente vers la PSRAM
  // que si `size > malloc_alwaysinternal_limit` (4 095, pose par main.cpp:130).
  // Un lot de 4 095 o ou MOINS repartirait donc dans le tas 8 bits INTERNE,
  // celui qui est affame (bug-080). Decouper « plus fin pour faire plus sur »
  // est donc exactement le mauvais reflexe, et cette assertion est la pour
  // l'arreter.
  const long budget = jsChunkBudget();
  char msg[520];
  snprintf(msg, sizeof(msg),
      "BUDGET TROP PETIT : %ld o <= kPsramExtmemThresholdBytes (%u). Un lot de "
      "cette taille est alloue dans le tas 8 BITS INTERNE, celui qui est en "
      "famine — l'inverse de l'effet cherche. Le budget doit rester AU-DESSUS "
      "du seuil, a dessein.", budget, (unsigned)kPsramExtmemThresholdBytes);
  TEST_ASSERT_TRUE_MESSAGE(budget > (long)kPsramExtmemThresholdBytes, msg);
}

static void test_parite_un_point_du_pire_cas_tient_toujours_dans_un_lot(void) {
  // Un point SEUL plus lourd que le budget partirait seul dans son lot et
  // serait refuse : l'import serait DEFINITIVEMENT bloque, sans recours.
  // Poids mesures au node du poste sur test/fixtures/history-worst-bytes.csv :
  //   · 148 o = la charge REELLEMENT postee aujourd'hui (7 cles), 17 chiffres
  //     significatifs deployes — le pire cas ATTEIGNABLE ;
  //   · 212 o = la MEME charge augmentee de phFiltered/orpFiltered, champs qui
  //     existent dans DataPoint (feature-095) mais ne sont ni exportes ni relus.
  // On borne contre 212, pas 148 : le jour ou les filtrees rejoignent l'import,
  // l'invariant doit deja tenir.
  const long budget = jsChunkBudget();
  const long worstPoint = 212;
  const long envelope = 31;
  char msg[480];
  snprintf(msg, sizeof(msg),
      "Le pire point (%ld o) plus l'enveloppe (%ld o) doit tenir dans un lot "
      "(%ld o). Sinon un point unique forme un lot hors gabarit, refuse en 413, "
      "et l'import est bloque sans recours possible cote client.",
      worstPoint, envelope, budget);
  TEST_ASSERT_TRUE_MESSAGE(envelope + worstPoint < budget, msg);
}

static void test_sentinelle_client_decoupe_par_OCTETS_MESURES(void) {
  // Le defaut d'origine : un decoupage « par N points » repasse le plafond au
  // premier fichier a decimales longues (104 a 148 o/point mesures sur les
  // fixtures, soit un facteur 1,4 selon le seul rendu des nombres).
  char* body = codeOfFunction("data/app.js", "function chunkHistoryPoints(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "jsonByteLength(JSON.stringify(point))") >= 1,
      "chunkHistoryPoints() doit MESURER les octets de chaque point serialise. "
      "Un decoupage par NOMBRE de points depasse le plafond des qu'un fichier "
      "porte des decimales longues (148 o/point contre 104) — c'est le mur que "
      "ce correctif retire.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "budgetBytes") >= 1,
      "Le budget d'octets doit etre le critere de coupe.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "overhead") >= 1,
      "L'enveloppe JSON doit etre comptee dans le budget : l'oublier laisse "
      "31 o depasser le plafond sur un lot plein.");
  free(body);
}

static void test_sentinelle_client_ne_remplace_qu_au_PREMIER_lot(void) {
  // ⚠️ SECOND point de destruction de donnees du lot, cote client. Si chaque
  // lot partait en `replace`, une restauration de 339 points en 6 lots ne
  // laisserait que le DERNIER (~60 points) : l'utilisateur verrait « import
  // termine » et aurait perdu 82 pourcent de sa sauvegarde.
  char* body = codeOfFunction("data/app.js", "async function sendHistoryChunks(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "(i === 0) ? state.mode : 'append'") >= 1,
      "Le mode demande ne s'applique qu'au PREMIER lot ; tous les suivants "
      "doivent partir en 'append'. Sans cela, chaque lot efface le precedent "
      "(history.cpp : memoryBuffer = dataPoints) et il ne reste que le dernier.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "state.index = i + 1") >= 1,
      "L'index de reprise doit avancer APRES le succes du lot : avance avant, "
      "une reprise sauterait le lot qui vient d'echouer.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "await authFetch(") >= 1,
      "Envoi SEQUENTIEL (await) : deux lots en vol se disputeraient le tampon "
      "de requete et le mutex de l'historique.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "413") >= 1 &&
                           countOccurrences(body, "503") >= 1,
      "Le client doit distinguer 413 (lot hors gabarit -> recharger la page) de "
      "503 (famine -> reessayer plus tard). Un message unique renverrait "
      "l'utilisateur vers la mauvaise action.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "body?.dropped") >= 1,
      "Le client doit cumuler `dropped` pour pouvoir le DIRE : c'est ce qui "
      "clot le defaut de perte muette de bug-101.");
  free(body);
}

// =============================================================================
// PARTIE 8 — INTÉGRITÉ DU JEU DE FIXTURES
// =============================================================================
// Les fixtures sont l'entrée du critère d'acceptation on-target. Une fixture qui
// pourrit (plafonds déplacés, colonne renommée) rend la recette on-target
// fausse sans que rien ne rougisse. Elles se régénèrent par
// `python3 tools/gen_history_fixture.py`.
// ⚠️ Elles sont SYNTHÉTIQUES. La sauvegarde réelle du 2026-09-05 vit hors du
// dépôt (/root/pool-backups/) et contient l'IP du broker MQTT et le SSID : elle
// a servi à CALIBRER ces formes, jamais à les remplir.

static char* readFixture(const char* name) {
  char rel[256];
  snprintf(rel, sizeof(rel), "test/fixtures/%s", name);
  char* txt = readProjectFile(rel, NULL);  // BRUT : le BOM et les CRLF comptent
  char msg[400];
  snprintf(msg, sizeof(msg),
      "Fixture %s introuvable. La regenerer : `python3 tools/gen_history_fixture.py`.",
      name);
  TEST_ASSERT_NOT_NULL_MESSAGE(txt, msg);
  return txt;
}

static size_t countDataRows(const char* txt) {
  size_t n = 0;
  for (const char* p = txt; *p != '\0'; ++p) if (*p == '\n') ++n;
  // La derniere ligne est terminee par un saut : n = 1 en-tete + lignes.
  return n > 0 ? n - 1 : 0;
}

// Compte les lignes dont la DERNIÈRE colonne (granularity) vaut `g`.
static size_t countByGranularity(const char* txt, char g, char delim) {
  size_t n = 0;
  const char* line = strchr(txt, '\n');
  if (line == NULL) return 0;
  ++line;
  while (*line != '\0') {
    const char* eol = strchr(line, '\n');
    const char* end = (eol != NULL) ? eol : line + strlen(line);
    // Recule jusqu'au dernier delimiteur avant la fin de ligne (CR compris).
    const char* q = end;
    while (q > line && (q[-1] == '\r' || q[-1] == ' ')) --q;
    const char* last = q;
    while (last > line && last[-1] != delim) --last;
    if (last < q && *last == g && (q - last) == 1) ++n;
    if (eol == NULL) break;
    line = eol + 1;
  }
  return n;
}

static void test_fixture_339_est_exactement_le_pire_cas_des_plafonds(void) {
  char* txt = readFixture("history-339.csv");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(339, countDataRows(txt),
      "history-339.csv doit porter EXACTEMENT 339 lignes de donnees : c'est le "
      "maximum STOCKABLE (96 + 168 + 75). Au-dela, rien de plus n'est stockable, "
      "donc c'est le seul jeu qui prouve « import complet, dropped = 0 ».");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(kMaxRawDataPoints, countByGranularity(txt, '0', ','),
      "96 points RAW attendus (kMaxRawDataPoints). Si le plafond a bouge dans "
      "constants.h, regenerer : `python3 tools/gen_history_fixture.py`.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(kMaxHourlyDataPoints, countByGranularity(txt, '1', ','),
      "168 points HOURLY attendus (kMaxHourlyDataPoints).");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(kMaxDailyDataPoints, countByGranularity(txt, '2', ','),
      "75 points DAILY attendus (kMaxDailyDataPoints).");
  free(txt);
}

static void test_fixture_339_porte_l_entete_exact_de_l_export(void) {
  // L'en-tete est le CONTRAT entre `bindHistoryBackup` (export) et
  // `parseHistoryCsv` (import). Une colonne renommee d'un cote casse le
  // aller-retour sans rien casser a la compilation.
  char* txt = readFixture("history-339.csv");
  const char* expected =
      "datetime,ph,orp,temperature,filtration,dosing,dosing_mask,granularity";
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(txt, expected, strlen(expected)),
      "En-tete de history-339.csv different de celui de bindHistoryBackup "
      "(data/app.js). Les fixtures doivent porter le format EXACT de l'export, "
      "sinon la recette on-target ne teste pas le fichier que l'utilisateur "
      "produit reellement.");
  free(txt);
}

static void test_fixture_overflow_deborde_bien_le_plafond_horaire(void) {
  char* txt = readFixture("history-overflow.csv");
  const size_t rows = countDataRows(txt);
  TEST_ASSERT_TRUE_MESSAGE(rows > kMaxHourlyDataPoints,
      "history-overflow.csv doit porter PLUS de points HOURLY que le plafond, "
      "sinon elle ne teste plus le rapport de `dropped` et l'invariant « une "
      "perte n'est jamais muette » cesse d'etre couvert.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(rows, countByGranularity(txt, '1', ','),
      "history-overflow.csv ne doit contenir QUE des HOURLY : melanger les "
      "granularites diluerait le debordement sur trois plafonds.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(32, rows - kMaxHourlyDataPoints,
      "Debordement attendu : 200 - 168 = 32 evinces. C'est le `dropped` que la "
      "recette on-target doit lire dans la reponse de la route.");
  free(txt);
}

static void test_fixture_legacy_n_a_PAS_de_colonne_dosing_mask(void) {
  // ⚠️ ATTENDU 0x04, JAMAIS 0x01. Un export <= 2.46 ne porte qu'un booleen
  // `dosing` : il dit qu'il y a EU dosage, jamais LEQUEL. Le relire 0x01
  // accuserait la pompe d'ACIDE d'injections de chlore — donnee fausse ET
  // favorable, que consolidateData() figerait 75 jours dans les DAILY. C'est le
  // defaut clos par bug-098, et cette fixture est ce qui empeche sa recidive
  // par le chemin de l'import.
  char* txt = readFixture("history-legacy.csv");
  const char* eol = strchr(txt, '\n');
  TEST_ASSERT_NOT_NULL(eol);
  const size_t headLen = (size_t)(eol - txt);
  char head[512];
  snprintf(head, sizeof(head), "%.*s", (int)headLen, txt);
  TEST_ASSERT_NULL_MESSAGE(strstr(head, "dosing_mask"),
      "history-legacy.csv NE DOIT PAS porter de colonne `dosing_mask` : c'est "
      "un export <= 2.46, et toute sa raison d'etre est de forcer le chemin "
      "« booleen seul -> voie INDETERMINEE (0x04) ».");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(head, "dosing"),
      "Elle doit en revanche porter la colonne `dosing` (booleen d'heritage).");
  free(txt);
}

static void test_sentinelle_client_lit_un_dosing_booleen_en_voie_INDETERMINEE(void) {
  // Le pendant CODE de la fixture ci-dessus : sans cette assertion, la fixture
  // ne serait qu'un fichier.
  char* body = codeOfFunction("data/app.js", "function parseHistoryCsv(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "DOSE_UNKNOWN") >= 1,
      "parseHistoryCsv() doit mapper un `dosing` booleen sans `dosing_mask` sur "
      "DOSE_UNKNOWN (0x04, voie indeterminee) — JAMAIS sur DOSE_PH (0x01), qui "
      "attribuerait a la pompe d'ACIDE des injections de chlore (bug-098).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "idxDosingMask") >= 1,
      "parseHistoryCsv() doit distinguer la colonne `dosing_mask` (>= 2.47.0) de "
      "la colonne `dosing` (<= 2.46) : avec le meme nom, un « 1 » serait "
      "indecidable — pH ou voie inconnue ?");
  char* src = readProjectCode("data/app.js");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(src, "DOSE_UNKNOWN = 0x04") >= 1,
      "DOSE_UNKNOWN doit valoir 0x04 cote client, comme kHistDoseUnknownChannel "
      "cote firmware. Une divergence renommerait la voie en silence.");
  free(src);
  free(body);
}

static void test_sentinelle_firmware_porte_le_meme_masque_indetermine(void) {
  char* src = readProjectCode("src/history.h");
  TEST_ASSERT_NOT_NULL(src);
  TEST_ASSERT_TRUE_MESSAGE(
      countOccurrences(src, "kHistDoseUnknownChannel = 0x04") >= 1,
      "kHistDoseUnknownChannel doit valoir 0x04 dans src/history.h, en parite "
      "avec DOSE_UNKNOWN de data/app.js. Sans cette parite, un import hérité "
      "designe une voie differente de chaque cote.");
  free(src);
}

static void test_fixture_hostile_porte_bien_ses_hostilites(void) {
  // Une fixture « hostile » qui aurait perdu son BOM, son `;` ou ses CRLF ne
  // testerait plus rien : c'est un fichier normal, et il passerait.
  char* txt = readFixture("history-hostile.csv");
  TEST_ASSERT_TRUE_MESSAGE((unsigned char)txt[0] == 0xEF &&
                           (unsigned char)txt[1] == 0xBB &&
                           (unsigned char)txt[2] == 0xBF,
      "history-hostile.csv doit COMMENCER par un BOM UTF-8 (EF BB BF). Sans "
      "lui, le premier nom de colonne devient « \\uFEFFdatetime » et TOUTES les "
      "colonnes sont perdues — c'est ce que produit un Excel Windows.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "\r\n"),
      "Fins de ligne CRLF attendues.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "datetime;ph"),
      "Delimiteur `;` attendu dans l'en-tete (sortie d'un Excel en locale FR).");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "7,15"),
      "Virgule decimale attendue : avec `;` comme delimiteur elle n'est pas "
      "ambigue, et c'est exactement ce qu'un tableur francais reecrit.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, ";;;;"),
      "Cellules VIDES attendues : une cellule vide relue comme 0 fabriquerait "
      "un pH 0 dans l'historique.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "pas-une-date"),
      "Une ligne au datetime ILLISIBLE doit figurer : elle doit etre SAUTEE, "
      "jamais provoquer un point a timestamp 0 (1970) ni un plantage.");
  free(txt);
}

static void test_sentinelle_client_tolere_le_BOM_et_les_delimiteurs(void) {
  char* body = codeOfFunction("data/app.js", "function parseHistoryCsv(");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "\\uFEFF") >= 1,
      "parseHistoryCsv() doit retirer le BOM UTF-8 : sinon le premier nom de "
      "colonne devient « \\uFEFFdatetime » et l'index de datetime vaut -1, donc "
      "AUCUNE ligne n'a de timestamp et le fichier s'importe VIDE — en "
      "annoncant un succes (fixture history-hostile.csv).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "\";\"") >= 1,
      "Le delimiteur `;` doit etre detecte (Excel en locale francaise).");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "replace(\",\", \".\")") >= 1,
      "La virgule decimale doit etre normalisee en point.");
  free(body);
}

static void test_fixture_245_reconstitue_la_distribution_du_cas_reel(void) {
  char* txt = readFixture("history-245.csv");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(245, countDataRows(txt),
      "history-245.csv doit porter 245 lignes : la taille du cas reel du "
      "05/09/2026, celui qui a mis bug-101 en evidence.");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(77, countByGranularity(txt, '0', ','),
      "77 RAW, comme la sauvegarde reelle (relevee hors depot, non versionnee).");
  TEST_ASSERT_EQUAL_UINT_MESSAGE(kMaxHourlyDataPoints, countByGranularity(txt, '1', ','),
      "168 HOURLY, comme la sauvegarde reelle : le plafond horaire etait ATTEINT.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, ",,,"),
      "Des points aux trois mesures ABSENTES doivent figurer (porte de mesure "
      "fermee) : c'est la que l'import se casse le plus souvent, une cellule "
      "vide relue comme 0 fabriquant un pH 0.");
  free(txt);
}

static void test_fixture_worst_bytes_force_bien_les_decimales_longues(void) {
  char* txt = readFixture("history-worst-bytes.csv");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "7.123456789012345"),
      "history-worst-bytes.csv doit porter des decimales a 17 chiffres "
      "significatifs : c'est ce qui fait passer un point de 104 a 148 octets et "
      "qui condamne tout decoupage « par nombre de points ».");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "ph_filtered"),
      "Les colonnes filtrees doivent figurer : ignorees par parseHistoryCsv "
      "aujourd'hui (donc 148 o/point), elles portent le pire cas a 212 o le jour "
      "ou elles rejoindront la charge. La borne du test vise 212, pas 148.");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, ",7,"),
      "Le masque de dosage le plus long (7 = pH|ORP|indetermine) doit figurer.");
  free(txt);
}

static void test_fixtures_ne_contiennent_aucun_secret(void) {
  // ⚠️ La sauvegarde reelle contient l'IP du broker MQTT et le SSID. Elle a
  // servi a CALIBRER ces fixtures ; rien ne doit en avoir ete recopie. Cette
  // assertion est le garde-fou de cette regle, pas une formalite : un « ah je
  // colle juste le vrai fichier pour aller vite » se voit ici.
  const char* names[6] = {"history-339.csv", "history-245.csv",
                          "history-worst-bytes.csv", "history-overflow.csv",
                          "history-legacy.csv", "history-hostile.csv"};
  const char* forbidden[6] = {"ssid", "SSID", "mqtt", "MQTT", "password", "192.168."};
  for (int i = 0; i < 6; ++i) {
    char* txt = readFixture(names[i]);
    for (int j = 0; j < 6; ++j) {
      char msg[420];
      snprintf(msg, sizeof(msg),
          "La fixture %s contient « %s ». Les fixtures versionnees doivent etre "
          "100 pourcent SYNTHETIQUES : la sauvegarde reelle "
          "(/root/pool-backups/) porte l'IP du broker MQTT et le SSID, et ne "
          "doit JAMAIS entrer dans le depot.", names[i], forbidden[j]);
      TEST_ASSERT_NULL_MESSAGE(strstr(txt, forbidden[j]), msg);
    }
    free(txt);
  }
}

// =============================================================================

int main(int, char**) {
  UNITY_BEGIN();

  // Partie 0 — l'outillage AVANT ce qui s'en sert
  RUN_TEST(test_outillage_le_depouillement_desarme_la_faille_du_commentaire);
  RUN_TEST(test_outillage_les_gabarits_backtick_ne_sont_pas_pris_pour_des_commentaires);
  RUN_TEST(test_outillage_extraction_par_signature_isole_bien_la_fonction);

  // Partie 1 — mode
  RUN_TEST(test_mode_absent_vaut_remplacer);
  RUN_TEST(test_mode_vide_vaut_remplacer);
  RUN_TEST(test_mode_replace_vaut_remplacer);
  RUN_TEST(test_mode_append_est_le_seul_a_ajouter);
  RUN_TEST(test_mode_append_toutes_casses_ascii_ajoutent);
  RUN_TEST(test_mode_inconnu_retombe_sur_remplacer);
  RUN_TEST(test_mode_aucune_mutation_d_un_caractere_d_append_n_ajoute);
  RUN_TEST(test_mode_est_une_fonction_pure_donc_stable);

  // Partie 2 — collision
  RUN_TEST(test_sameslot_meme_creneau_toutes_granularites);
  RUN_TEST(test_sameslot_couvre_explicitement_raw);
  RUN_TEST(test_sameslot_granularites_differentes_au_meme_timestamp);
  RUN_TEST(test_sameslot_creneaux_voisins_sont_distincts);
  RUN_TEST(test_sameslot_matrice_exhaustive_et_symetrique);
  RUN_TEST(test_sameslot_bornes_uint32_sans_repliement);

  // Partie 3 — bornage
  RUN_TEST(test_trim_sous_les_plafonds_ne_purge_rien);
  RUN_TEST(test_trim_historique_vide_aucun_underflow);
  RUN_TEST(test_trim_exactement_au_plafond_ne_purge_rien);
  RUN_TEST(test_trim_un_seul_point_au_dela_du_plafond);
  RUN_TEST(test_trim_les_trois_granularites_sont_independantes);
  RUN_TEST(test_trim_le_plan_est_la_difference_exacte);
  RUN_TEST(test_trim_les_plafonds_viennent_de_constants_h);

  // Partie 4 — idempotence
  RUN_TEST(test_fusion_pire_cas_339_entre_intact);
  RUN_TEST(test_fusion_idempotence_exacte_du_meme_lot);
  RUN_TEST(test_fusion_idempotence_tient_sur_dix_reenvois);
  RUN_TEST(test_fusion_lot_raw_reenvoye_ne_double_pas);
  RUN_TEST(test_fusion_ecrase_bien_avec_la_valeur_entrante);
  RUN_TEST(test_fusion_ecrase_TOUTES_les_occurrences_d_un_creneau);
  RUN_TEST(test_fusion_variante_fautive_premiere_occurrence_perd_l_invariant);
  RUN_TEST(test_fusion_eviction_garde_les_points_les_plus_RECENTS);
  RUN_TEST(test_fusion_variante_fautive_eviction_des_recents_est_detectee);
  RUN_TEST(test_fusion_lot_recent_dans_un_historique_plein);
  RUN_TEST(test_fusion_lot_portant_deux_fois_le_meme_creneau);
  RUN_TEST(test_fusion_lots_successifs_equivalent_a_un_lot_unique);
  RUN_TEST(test_fusion_reprise_apres_coupure_ne_cree_pas_de_doublon);

  // Partie 5 — câblage coquille
  RUN_TEST(test_sentinelle_appendData_delegue_la_regle_de_collision);
  RUN_TEST(test_sentinelle_appendData_n_exclut_pas_RAW);
  RUN_TEST(test_sentinelle_appendData_ecrase_toutes_les_occurrences);
  RUN_TEST(test_sentinelle_appendData_borne_et_ne_moyenne_pas);
  RUN_TEST(test_sentinelle_appendData_rapporte_total_et_dropped);
  RUN_TEST(test_sentinelle_trimToCaps_delegue_le_plan_et_purge_les_anciens);
  RUN_TEST(test_sentinelle_non_regression_recordDataPoint_intact);
  RUN_TEST(test_sentinelle_non_regression_consolidateData_intact);
  RUN_TEST(test_sentinelle_sizeof_datapoint_reste_verrouille_a_28);
  RUN_TEST(test_sentinelle_enum_granularite_inchange);

  // Partie 6 — garde de corps JSON
  RUN_TEST(test_sentinelle_garde_compare_strictement_le_plafond);
  RUN_TEST(test_sentinelle_garde_rend_413_et_503_actionnables);
  RUN_TEST(test_sentinelle_garde_n_alloue_ni_ne_journalise_sur_async_tcp);
  RUN_TEST(test_sentinelle_route_import_gardee_avant_son_handler);
  RUN_TEST(test_sentinelle_routes_wifi_et_capteur_gardees_avant_leur_handler);
  RUN_TEST(test_sentinelle_route_import_aiguille_par_la_regle_de_mode);
  RUN_TEST(test_sentinelle_plafonds_de_corps_restent_sous_le_defaut_bibliotheque);

  // Partie 7 — parité client/route
  RUN_TEST(test_parite_budget_client_strictement_sous_le_plafond_de_route);
  RUN_TEST(test_parite_budget_client_egale_la_constante_firmware);
  RUN_TEST(test_parite_marge_de_1024_octets_entre_budget_et_plafond);
  RUN_TEST(test_sentinelle_le_decoupage_mesure_les_octets_et_ne_divise_pas);
  RUN_TEST(test_parite_budget_client_reste_au_dessus_du_seuil_psram);
  RUN_TEST(test_parite_un_point_du_pire_cas_tient_toujours_dans_un_lot);
  RUN_TEST(test_sentinelle_client_decoupe_par_OCTETS_MESURES);
  RUN_TEST(test_sentinelle_client_ne_remplace_qu_au_PREMIER_lot);

  // Partie 8 — fixtures
  RUN_TEST(test_fixture_339_est_exactement_le_pire_cas_des_plafonds);
  RUN_TEST(test_fixture_339_porte_l_entete_exact_de_l_export);
  RUN_TEST(test_fixture_overflow_deborde_bien_le_plafond_horaire);
  RUN_TEST(test_fixture_legacy_n_a_PAS_de_colonne_dosing_mask);
  RUN_TEST(test_sentinelle_client_lit_un_dosing_booleen_en_voie_INDETERMINEE);
  RUN_TEST(test_sentinelle_firmware_porte_le_meme_masque_indetermine);
  RUN_TEST(test_fixture_hostile_porte_bien_ses_hostilites);
  RUN_TEST(test_sentinelle_client_tolere_le_BOM_et_les_delimiteurs);
  RUN_TEST(test_fixture_245_reconstitue_la_distribution_du_cas_reel);
  RUN_TEST(test_fixture_worst_bytes_force_bien_les_decimales_longues);
  RUN_TEST(test_fixtures_ne_contiennent_aucun_secret);

  return UNITY_END();
}
