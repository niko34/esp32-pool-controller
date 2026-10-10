// =============================================================================
// Tests natifs — feature-149 : PLUS DE MESURE FIGÉE DANS LES CAUSES DE GARDE pH
// =============================================================================
// CE QUE CE LOT PROTÈGE
// --------------------
// Les huit appels à `setGuardRefusalCause()` de src/pump_controller.cpp ne
// doivent plus jamais citer une MESURE (elle se périme dès l'edge-trigger qui
// la pose — incident de production du 2026-10-10, carte à 6,67 / bandeau
// « 6,70 sous 6,70 »). Seuls les SEUILS (constantes de constants.h) peuvent
// apparaître, avec UNE décimale. Les relâches et `kPhGuardReleaseHysteresis`
// restent, eux, à DEUX décimales (ils ne sont pas ronds au dixième).
//
// POURQUOI CE TEST LIT LE TEXTE DES SOURCES
// ------------------------------------------
// `pump_controller.cpp` n'est pas compilable en natif (FreeRTOS, EZO I2C,
// ArduinoJson, LittleFS). Patron imposé, identique à test_native_dose_mask,
// test_native_ws_data_parity et test_native_dose_banner_silence :
// `readProjectFile()` + analyse du TEXTE.
//
// L'OPTION ÉCARTÉE (et pourquoi elle l'est) : extraire les gabarits dans un
// en-tête pur mesurerait les gabarits, pas les SITES D'APPEL — un
// `+ String(ph, 2)` ajouté au site d'appel resterait invisible. C'est
// précisément la mutation que ce gate doit tuer (m2 ci-dessous).
//
// MÉCANIQUE DE L'ANALYSEUR — pour CHAQUE appel à `setGuardRefusalCause(` dont
// le premier argument est `0` ou `1` (ce qui exclut la DÉFINITION de la
// méthode, ligne ~759, dont le premier paramètre est `int pumpIndex`) :
//   - un littéral `"..."` → ses octets sont copiés TELS QUELS (concaténation
//     `+` multi-lignes comprise) ;
//   - un `String(<ident>, <n>)` → si `<ident>` n'est PAS dans la liste
//     blanche (kPhFloorCorrected, kPhFloorRawHard, kPhCeilingCorrected,
//     kPhCeilingRawHard, kPhOrpInhibit) ⇒ ÉCHEC « une mesure a été
//     réintroduite » ; sinon la valeur réelle (LUE dans constants.h via le
//     symbole C++, jamais recopiée à la main) est substituée via
//     `snprintf("%.*f", n, valeur)` ;
//   - un TERNAIRE (`cond ? A : B`, site ORP ~1318) ouvre DEUX variantes,
//     mesurées et classifiées séparément ;
//   - tout autre token (un identifiant quelconque, un appel de fonction, un
//     opérateur non reconnu) ⇒ ÉCHEC fail-closed : un futur `+ foo()` ne doit
//     JAMAIS passer en silence.
// =============================================================================

#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

// constants.h est self-contained (stddef.h/stdint.h/time.h uniquement) : on y
// lit les VRAIES valeurs de seuil, jamais recopiées à la main ici.
#include "constants.h"

// =============================================================================
// T4 — SENTINELLE UTF-8, EN TÊTE DE SUITE
// =============================================================================
// Si ceci casse, quelqu'un a remplacé la mesure par un comptage de
// CARACTÈRES. L'écart octets/caractères est de 4 à 5 sur les textes de ce
// fichier (accents, tirets cadratins « — ») : une mesure en caractères
// laisserait passer un texte de 99 à 101 caractères là où le budget réel est
// de 95 OCTETS — soit un texte tronqué en production (c'est très exactement
// ce qui s'est produit pour le site 994/1022, cf. spec feature-149, addendum 2).
void test_sentinelle_utf8_strlen_compte_des_octets(void) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)strlen("é"),
      "« é » doit peser 2 octets UTF-8 : si ceci vaut 1, strlen() compte des "
      "caracteres, pas des octets, et tout le budget de ce fichier est faux");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, (int)strlen("—"),
      "« — » (tiret cadratin) doit peser 3 octets UTF-8");
}

// =============================================================================
// Lecture des sources (patron test_native_dose_mask / test_native_dose_banner_silence)
// =============================================================================

static void stripComments(char* s);

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  const char* candidates[2];

  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_ph_guard_causes/");
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

// Neutralise les commentaires (remplacés par des espaces), conserve les
// littéraux de chaîne/caractère intacts — indispensable : les commentaires de
// cette spec CITENT des textes fautifs ("String(ph, 2)") qu'un test naïf
// prendrait pour du code.
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

// =============================================================================
// L'ANALYSEUR : un mini-interpréteur d'expression C++ « + »/ternaire/String()
// =============================================================================

struct Analysis {
  bool ok = true;
  std::string failReason;      // vide si ok
  bool measureReintroduced = false;
  std::vector<std::string> variantTexts;  // 1 par variante (2 pour un ternaire)
  std::vector<int> variantDecimalsUsed;   // un seul `n` attendu par variante normalement,
                                          // mais on garde TOUS les n vus (pour T3)
  std::vector<int> allDecimalsSeen;
};

static bool lookupThreshold(const std::string& ident, double* outVal) {
  // Liste blanche EXHAUSTIVE (feature-149) : tout autre identifiant est une
  // MESURE (ph, phRaw, offset...) et doit faire échouer l'analyse.
  if (ident == "kPhFloorCorrected")  { *outVal = (double)kPhFloorCorrected;  return true; }
  if (ident == "kPhFloorRawHard")    { *outVal = (double)kPhFloorRawHard;    return true; }
  if (ident == "kPhCeilingCorrected"){ *outVal = (double)kPhCeilingCorrected; return true; }
  if (ident == "kPhCeilingRawHard")  { *outVal = (double)kPhCeilingRawHard;  return true; }
  if (ident == "kPhOrpInhibit")      { *outVal = (double)kPhOrpInhibit;     return true; }
  return false;
}

static bool containsDigit(const std::string& s) {
  for (char c : s) if (c >= '0' && c <= '9') return true;
  return false;
}

static std::string trimCopy(const char* begin, const char* end) {
  while (begin < end && (*begin == ' ' || *begin == '\n' || *begin == '\t' || *begin == '\r')) begin++;
  while (end > begin && (*(end - 1) == ' ' || *(end - 1) == '\n' || *(end - 1) == '\t' || *(end - 1) == '\r')) end--;
  return std::string(begin, end);
}

// Découpe `expr` (sans les ternaires) en termes séparés par des `+` DE
// PREMIER NIVEAU (hors guillemets, hors parenthèses — ces dernières
// n'apparaissent ici que dans String(...), qui ne contient jamais de `+`).
static std::vector<std::string> splitTopLevelPlus(const std::string& expr) {
  std::vector<std::string> out;
  int depth = 0;
  bool inStr = false;
  size_t start = 0;
  for (size_t i = 0; i < expr.size(); ++i) {
    char c = expr[i];
    if (inStr) {
      if (c == '\\' && i + 1 < expr.size()) { i++; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (c == '(') { depth++; continue; }
    if (c == ')') { depth--; continue; }
    if (c == '+' && depth == 0) {
      out.push_back(trimCopy(expr.c_str() + start, expr.c_str() + i));
      start = i + 1;
    }
  }
  out.push_back(trimCopy(expr.c_str() + start, expr.c_str() + expr.size()));
  return out;
}

// Classe un terme unique (déjà trimmé) : littéral de chaîne, ou String(id, n).
// Retourne false (fail-closed) sur tout autre token.
static bool classifyTerm(const std::string& term, Analysis* a, std::string* outBytes) {
  if (term.empty()) { a->ok = false; a->failReason = "terme vide"; return false; }

  if (term[0] == '"') {
    // Littéral : extrait le contenu entre guillemets (doit couvrir TOUT le terme).
    size_t i = 1;
    std::string content;
    while (i < term.size() && term[i] != '"') {
      if (term[i] == '\\' && i + 1 < term.size()) {
        content.push_back(term[i + 1]);
        i += 2;
      } else {
        content.push_back(term[i]);
        i++;
      }
    }
    if (i >= term.size() || term[i] != '"' || i != term.size() - 1) {
      a->ok = false;
      a->failReason = "litteral mal forme ou terme compose (token inconnu) : " + term;
      return false;
    }
    if (containsDigit(content)) {
      a->ok = false;
      a->failReason = "un chiffre est present dans un fragment litteral (seuil "
                       "recopie en dur ?) : \"" + content + "\"";
      return false;
    }
    *outBytes = content;
    return true;
  }

  // Attendu : String ( ident , n )
  size_t p = term.find("String");
  if (p != 0) {
    a->ok = false;
    a->failReason = "token non reconnu (fail-closed) : " + term;
    return false;
  }
  size_t paren = term.find('(', p);
  if (paren == std::string::npos || term.back() != ')') {
    a->ok = false;
    a->failReason = "appel String(...) mal forme : " + term;
    return false;
  }
  std::string inner = term.substr(paren + 1, term.size() - paren - 2);
  size_t comma = inner.find(',');
  if (comma == std::string::npos) {
    a->ok = false;
    a->failReason = "String(...) sans virgule (pas de nombre de decimales) : " + term;
    return false;
  }
  std::string ident = trimCopy(inner.c_str(), inner.c_str() + comma);
  std::string nStr = trimCopy(inner.c_str() + comma + 1, inner.c_str() + inner.size());
  if (nStr.empty() || nStr.find_first_not_of("0123456789") != std::string::npos) {
    a->ok = false;
    a->failReason = "nombre de decimales illisible dans : " + term;
    return false;
  }
  int n = atoi(nStr.c_str());

  double val = 0.0;
  if (!lookupThreshold(ident, &val)) {
    a->ok = false;
    a->measureReintroduced = true;
    a->failReason = "une mesure a ete reintroduite : identifiant hors liste "
                     "blanche dans la cause de garde : " + ident;
    return false;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f", n, val);
  *outBytes = std::string(buf);
  a->allDecimalsSeen.push_back(n);
  a->variantDecimalsUsed.push_back(n);
  return true;
}

// Analyse une expression (sans ternaire) : concatène les octets substitués de
// chaque terme de premier niveau.
static bool analyzeFlatExpr(const std::string& expr, Analysis* a, std::string* outText) {
  std::vector<std::string> terms = splitTopLevelPlus(expr);
  std::string acc;
  for (const std::string& t : terms) {
    std::string bytes;
    if (!classifyTerm(t, a, &bytes)) return false;
    acc += bytes;
  }
  *outText = acc;
  return true;
}

// Analyse l'expression COMPLÈTE d'un appel (2e argument de
// setGuardRefusalCause) : repère un éventuel ternaire de premier niveau et
// produit UNE OU DEUX variantes.
static Analysis analyzeCauseExpr(const std::string& expr) {
  Analysis a;

  // Repère un `?` de premier niveau (hors guillemets/parens).
  int depth = 0;
  bool inStr = false;
  long qPos = -1, cPos = -1;
  for (size_t i = 0; i < expr.size(); ++i) {
    char c = expr[i];
    if (inStr) {
      if (c == '\\' && i + 1 < expr.size()) { i++; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (c == '(') { depth++; continue; }
    if (c == ')') { depth--; continue; }
    if (depth == 0 && c == '?' && qPos < 0) { qPos = (long)i; continue; }
    if (depth == 0 && c == ':' && qPos >= 0 && cPos < 0) { cPos = (long)i; continue; }
  }

  if (qPos >= 0 && cPos >= 0) {
    // Ternaire : deux variantes, la condition elle-même n'est PAS du texte
    // affiché et n'est donc pas analysée ici.
    std::string thenExpr = trimCopy(expr.c_str() + qPos + 1, expr.c_str() + cPos);
    std::string elseExpr = trimCopy(expr.c_str() + cPos + 1, expr.c_str() + expr.size());
    std::string t1, t2;
    Analysis a1; if (!analyzeFlatExpr(thenExpr, &a1, &t1)) return a1;
    Analysis a2; if (!analyzeFlatExpr(elseExpr, &a2, &t2)) return a2;
    a.ok = true;
    a.variantTexts.push_back(t1);
    a.variantTexts.push_back(t2);
    a.allDecimalsSeen = a1.allDecimalsSeen;
    for (int d : a2.allDecimalsSeen) a.allDecimalsSeen.push_back(d);
    return a;
  }

  std::string t1;
  if (!analyzeFlatExpr(expr, &a, &t1)) return a;
  a.variantTexts.push_back(t1);
  return a;
}

// =============================================================================
// Extraction des HUIT appels à setGuardRefusalCause(0|1, <expr>)
// =============================================================================
// Exclut la DÉFINITION (`void PumpControllerClass::setGuardRefusalCause(int
// pumpIndex, const String& cause)`) : son premier argument n'est ni `0` ni `1`.

struct CallSite {
  std::string name;     // identité lisible, pour les messages d'échec
  std::string exprText; // le texte brut du 2e argument
};

static std::vector<CallSite> extractCallSites(const char* src, size_t len) {
  std::vector<CallSite> out;
  const char* anchor = "setGuardRefusalCause(";
  const size_t an = strlen(anchor);
  size_t i = 0;
  int siteNo = 0;
  while (i + an <= len) {
    if (memcmp(src + i, anchor, an) != 0) { i++; continue; }
    size_t p = i + an;
    // Premier argument : jusqu'à la première virgule de premier niveau.
    size_t argStart = p;
    while (p < len && src[p] != ',') p++;
    if (p >= len) { i++; continue; }
    std::string firstArg = trimCopy(src + argStart, src + p);
    if (firstArg != "0" && firstArg != "1") {
      // La définition de la méthode (int pumpIndex, ...), ou tout autre bruit.
      i = p + 1;
      continue;
    }
    // Deuxième argument : balance les parenthèses jusqu'à la fermeture de
    // l'appel (depth revient à -1 depuis 0), en ignorant guillemets.
    size_t exprStart = p + 1;
    size_t q = exprStart;
    int depth = 0;
    bool inStr = false;
    bool closed = false;
    for (; q < len; ++q) {
      char c = src[q];
      if (inStr) {
        if (c == '\\' && q + 1 < len) { q++; continue; }
        if (c == '"') inStr = false;
        continue;
      }
      if (c == '"') { inStr = true; continue; }
      if (c == '(') { depth++; continue; }
      if (c == ')') {
        if (depth == 0) { closed = true; break; }
        depth--;
        continue;
      }
    }
    if (!closed) { i = p + 1; continue; }
    std::string exprText = trimCopy(src + exprStart, src + q);
    char nameBuf[64];
    snprintf(nameBuf, sizeof(nameBuf), "site#%d (pompe=%s)", ++siteNo, firstArg.c_str());
    out.push_back(CallSite{std::string(nameBuf), exprText});
    i = q + 1;
  }
  return out;
}

// =============================================================================
// Fixture partagée : lit pump_controller.cpp UNE fois, extrait les 8 sites.
// =============================================================================

static std::vector<CallSite> g_sites;
static bool g_sitesLoaded = false;

static void ensureSitesLoaded(void) {
  if (g_sitesLoaded) return;
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/pump_controller.cpp introuvable");
  g_sites = extractCallSites(src, (size_t)len);
  free(src);
  g_sitesLoaded = true;
}

void setUp(void) { ensureSitesLoaded(); }
void tearDown(void) {}

// =============================================================================
// T0 — le RECENSEMENT lui-même : huit appels, ni plus ni moins
// =============================================================================

void test_recensement_huit_appels_exactement(void) {
  char msg[160];
  snprintf(msg, sizeof(msg),
      "attendu 8 appels a setGuardRefusalCause(0|1, ...), trouve %d — le "
      "recensement de la spec (922,952,1022,1059,1178,1318,1468,1501) a bouge",
      (int)g_sites.size());
  TEST_ASSERT_EQUAL_INT_MESSAGE(8, (int)g_sites.size(), msg);
}

// =============================================================================
// T1 — longueur en OCTETS apres substitution, <= 95 pour les HUIT causes
//       (les deux variantes du ternaire comptees separement)
// =============================================================================

void test_t1_longueur_apres_substitution_sous_95_octets(void) {
  for (const CallSite& site : g_sites) {
    Analysis a = analyzeCauseExpr(site.exprText);
    char ctxMsg[256];
    snprintf(ctxMsg, sizeof(ctxMsg), "%s : analyse en echec (%s)",
             site.name.c_str(), a.failReason.c_str());
    TEST_ASSERT_TRUE_MESSAGE(a.ok, ctxMsg);
    if (!a.ok) continue;
    for (size_t v = 0; v < a.variantTexts.size(); ++v) {
      const std::string& text = a.variantTexts[v];
      char lenMsg[320];
      snprintf(lenMsg, sizeof(lenMsg),
          "%s variante %zu depasse 95 octets (%zu o) : \"%s\"",
          site.name.c_str(), v, text.size(), text.c_str());
      TEST_ASSERT_TRUE_MESSAGE(text.size() <= 95, lenMsg);
    }
  }
}

// =============================================================================
// T2 — aucune mesure (identifiant hors liste blanche), aucun chiffre en dur
// =============================================================================

void test_t2_aucune_mesure_reintroduite(void) {
  for (const CallSite& site : g_sites) {
    Analysis a = analyzeCauseExpr(site.exprText);
    char msg[320];
    snprintf(msg, sizeof(msg), "%s : %s", site.name.c_str(), a.failReason.c_str());
    TEST_ASSERT_TRUE_MESSAGE(a.ok, msg);
    TEST_ASSERT_FALSE_MESSAGE(a.measureReintroduced,
        "une mesure a ete detectee alors que l'analyse devrait reussir");
  }
}

// =============================================================================
// T3 — les QUATRE causes pH utilisent , 1) ; relaches et hysteresis a , 2)
// =============================================================================

void test_t3_les_quatre_causes_ph_utilisent_une_decimale(void) {
  // Les quatre causes pH correspondent aux sites#1..4 dans l'ordre du fichier
  // (922, 952, 1022 [2 substitutions], 1059) — tous pompe=0, tous AVANT le
  // site « manuel » (1178, sans substitution) et les sites ORP (pompe=1).
  int nbAvecSubst = 0;
  for (const CallSite& site : g_sites) {
    if (site.name.find("pompe=0") == std::string::npos) continue;
    Analysis a = analyzeCauseExpr(site.exprText);
    char failMsg[320];
    snprintf(failMsg, sizeof(failMsg), "%s : %s", site.name.c_str(), a.failReason.c_str());
    TEST_ASSERT_TRUE_MESSAGE(a.ok, failMsg);
    if (a.allDecimalsSeen.empty()) continue;  // site 1178 : pur litteral, rien a verifier
    nbAvecSubst++;
    for (int n : a.allDecimalsSeen) {
      char msg[192];
      snprintf(msg, sizeof(msg),
          "%s : une cause de garde pH substitue encore %d decimales (attendu 1)",
          site.name.c_str(), n);
      TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, msg);
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, nbAvecSubst,
      "attendu exactement 4 causes pH (pompe=0) portant une substitution de seuil");
}

// -----------------------------------------------------------------------------
// Isole le texte d'UNE SEULE instruction (statement), bornee par un marqueur de
// DEBUT (ex. "systemLogger.info(", "band =") cherche en arriere depuis un
// ANCRE unique (une phrase qui n'apparait qu'a cet endroit), et par le premier
// ';' rencontre APRES l'ancre. C'est ce qui rend le test LOCAL au message : une
// occurrence de la meme constante ailleurs dans le fichier (ex. le message
// `critical` voisin, qui garde sciemment la mesure) ne peut plus le satisfaire
// a tort — c'est precisement le defaut signale par la revue (m4 « survivait »
// parce que la premiere version cherchait dans TOUT le fichier).
static std::string statementWindow(const std::string& body, const char* uniqueAnchor,
                                   const char* stmtStartMarker) {
  size_t pos = body.find(uniqueAnchor);
  if (pos == std::string::npos) return std::string();
  size_t start = body.rfind(stmtStartMarker, pos);
  if (start == std::string::npos) return std::string();
  size_t semi = body.find(';', pos);
  if (semi == std::string::npos) return std::string();
  return body.substr(start, semi + 1 - start);
}

// Releve TOUS les `String(<expr>, <n>)` d'une fenetre et renvoie la liste des
// `<n>` vus, dans l'ordre. Ne fait AUCUNE hypothese sur `<expr>` : un message
// de relache cite plusieurs choses (un seuil ET l'hysteresis) et TOUTES
// doivent rester a deux decimales, pas seulement celle qu'on visait au depart.
static std::vector<int> allStringDecimalsIn(const std::string& window) {
  std::vector<int> out;
  size_t pos = 0;
  while (true) {
    size_t p = window.find("String(", pos);
    if (p == std::string::npos) break;
    size_t close = window.find(')', p);
    if (close == std::string::npos) break;
    std::string inner = window.substr(p + 7, close - p - 7);
    size_t comma = inner.rfind(',');
    if (comma != std::string::npos) {
      std::string nStr = trimCopy(inner.c_str() + comma + 1, inner.c_str() + inner.size());
      if (!nStr.empty() && nStr.find_first_not_of("0123456789") == std::string::npos) {
        out.push_back(atoi(nStr.c_str()));
      }
    }
    pos = close + 1;
  }
  return out;
}

struct ReleaseSite {
  const char* uniqueAnchor;    // phrase unique dans TOUT le fichier
  const char* stmtStartMarker; // debut de l'instruction qui la porte
  const char* label;
};

// Les SIX endroits ou kPhGuardReleaseHysteresis (0,05 — non ronde au dixieme)
// intervient : les 4 relaches pH (bug-135/feature-149, exclusion explicite de
// la regle « une decimale ») + les 2 bandes d'hysteresis ORP, deja saines et
// hors perimetre de cette spec mais a proteger quand meme (revue).
static const ReleaseSite kReleaseSites[] = {
    {"butée brute haute levée", "systemLogger.info(",
     "relache butee brute haute (pH+)"},
    {"injection de pH+ de nouveau possible", "systemLogger.info(",
     "relache plafond corrige (pH+)"},
    {"butée brute levée", "systemLogger.info(",
     "relache butee brute basse (acide)"},
    {"injection d'acide de nouveau possible", "systemLogger.info(",
     "relache plancher corrige (acide)"},
    {"maintenu dans la bande d'hystérésis de la butée ", "band =",
     "bande ORP butee brute haute"},
    {"maintenu dans la bande d'hystérésis du seuil ", "band =",
     "bande ORP seuil corrige"},
};

void test_t3_relaches_et_hysteresis_restent_a_deux_decimales(void) {
  // Les RELÂCHES (messages systemLogger.info / bandes ORP) NE SONT PAS des
  // appels a setGuardRefusalCause : on les verifie directement sur le texte
  // source (comments stripped), pas via l'analyseur de causes — mais de
  // facon LOCALE a chaque message (voir statementWindow), pas sur le fichier
  // entier.
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  std::string body(src, (size_t)len);
  free(src);

  for (const ReleaseSite& site : kReleaseSites) {
    std::string window = statementWindow(body, site.uniqueAnchor, site.stmtStartMarker);
    char notFoundMsg[192];
    snprintf(notFoundMsg, sizeof(notFoundMsg),
        "%s : instruction introuvable (ancre ou marqueur de debut absent)",
        site.label);
    TEST_ASSERT_FALSE_MESSAGE(window.empty(), notFoundMsg);

    std::vector<int> decimals = allStringDecimalsIn(window);
    char emptyMsg[192];
    snprintf(emptyMsg, sizeof(emptyMsg),
        "%s : aucun String(..., n) trouve dans l'instruction isolee — fenetre "
        "mal bornee ?", site.label);
    TEST_ASSERT_TRUE_MESSAGE(!decimals.empty(), emptyMsg);

    for (int n : decimals) {
      char msg[256];
      snprintf(msg, sizeof(msg),
          "%s : une valeur de cette relache/bande est affichee a %d decimale(s) "
          "(attendu 2) — fenetre : \"%s\"",
          site.label, n, window.c_str());
      TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, msg);
    }
  }
}

// =============================================================================
// T5 — NON-VACUITE du helper numfmt::isRoundedToTenth
// =============================================================================
// Sans ce test, un `return true;` rendrait le static_assert de constants.h
// PUREMENT DECORATIF : il compilerait toujours, quel que soit kPhSafetyMargin.

void test_t5_is_rounded_to_tenth_non_vacuite(void) {
  TEST_ASSERT_FALSE_MESSAGE(numfmt::isRoundedToTenth(6.75f),
      "6.75 ne doit PAS passer pour rond au dixieme");
  TEST_ASSERT_TRUE_MESSAGE(numfmt::isRoundedToTenth(6.70f),
      "6.70 doit etre reconnu rond au dixieme");
  TEST_ASSERT_TRUE_MESSAGE(numfmt::isRoundedToTenth(8.10f),
      "8.10 doit etre reconnu rond au dixieme");
  TEST_ASSERT_TRUE_MESSAGE(numfmt::isRoundedToTenth(6.7000003f),
      "6.7000003 (bruit de representation float) doit rester accepte comme rond");
  TEST_ASSERT_FALSE_MESSAGE(numfmt::isRoundedToTenth(6.705f),
      "6.705 (a mi-chemin entre deux dixiemes) ne doit PAS passer pour rond");
}

void test_t5_static_assert_les_quatre_seuils_sont_ronds(void) {
  // Rejoue a l'execution ce que le static_assert de constants.h verifie a la
  // compilation — si jamais quelqu'un neutralisait le static_assert (#if 0),
  // ce test resterait un filet.
  TEST_ASSERT_TRUE(numfmt::isRoundedToTenth(kPhFloorCorrected));
  TEST_ASSERT_TRUE(numfmt::isRoundedToTenth(kPhFloorRawHard));
  TEST_ASSERT_TRUE(numfmt::isRoundedToTenth(kPhCeilingCorrected));
  TEST_ASSERT_TRUE(numfmt::isRoundedToTenth(kPhCeilingRawHard));
}

// =============================================================================
// T6 — NON-REGRESSION : discriminance, inconditionnalite, hysteresis, code 250
// =============================================================================

void test_t6_discriminance_site_994_1022_if_not_low(void) {
  // Le texte "Verifiez l'offset pH" (site pompe=0 #3, ligne ~1022) ne doit
  // etre pose QUE si la garde corrigee NE mord PAS (if (!low)) — condition
  // deja verrouillee par bug-135, non touchee par cette spec, mais dont la
  // disparition romprait le sens diagnostique de la ligne 994/1022.
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  std::string body(src, (size_t)len);
  free(src);

  size_t pos = body.find("Vérifiez l'offset pH");
  TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
      "le texte de diagnostic offset a disparu du fichier");
  // Les 200 caracteres precedents doivent contenir `if (!low)` sans
  // `} else {` ni `if (low)` entre les deux (garde directement englobante).
  size_t searchStart = pos > 220 ? pos - 220 : 0;
  std::string before = body.substr(searchStart, pos - searchStart);
  TEST_ASSERT_TRUE_MESSAGE(before.find("if (!low)") != std::string::npos,
      "le texte de diagnostic offset n'est plus garde par if (!low) : la "
      "discriminance (bug-135) a ete perdue");
}

void test_t6_inconditionnalite_cause_floor_corrigee(void) {
  // La cause "pH sous le plancher de securite" (site pompe=0 #4, ligne 1059)
  // doit rester INCONDITIONNELLE (pas de `if (!xxx)` l'entourant juste avant,
  // contrairement au site precedent) — c'est la valeur CORRIGEE qui fait foi.
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  std::string body(src, (size_t)len);
  free(src);

  size_t pos = body.find("pH sous le plancher de sécurité");
  TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos,
      "le texte de la cause plancher corrigee a disparu");
  size_t ifLowPos = body.rfind("if (low && !_phLowBlockActive)", pos);
  TEST_ASSERT_TRUE_MESSAGE(ifLowPos != std::string::npos && pos - ifLowPos < 2000,
      "le bloc `if (low && !_phLowBlockActive)` qui pose cette cause a disparu "
      "ou a ete deplace loin de son texte");
  // Aucun `if (!` ne doit se trouver ENTRE la condition du bloc et la pose de
  // la cause : elle est posee inconditionnellement DANS ce bloc.
  std::string between = body.substr(ifLowPos, pos - ifLowPos);
  TEST_ASSERT_TRUE_MESSAGE(between.find("if (!") == std::string::npos,
      "la cause du plancher corrige est desormais conditionnelle : elle doit "
      "rester inconditionnelle (priorite inversee de bug-135)");
}

void test_t6_hysteresis_relache_presente(void) {
  long len = 0;
  char* src = readProjectFile("src/pump_controller.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  std::string body(src, (size_t)len);
  free(src);

  TEST_ASSERT_TRUE_MESSAGE(body.find("kPhGuardReleaseHysteresis") != std::string::npos,
      "l'hysteresis de relache a disparu du fichier");
  // 6 usages CODE (comments stripped) : les 4 relaches pH (plancher/plafond
  // corriges, butees brutes basse/haute) + les 2 bandes d'hysteresis ORP
  // (kPhCeilingRawHard - kPhGuardReleaseHysteresis, kPhOrpInhibit -
  // kPhGuardReleaseHysteresis), toutes hors perimetre de cette spec (voie ORP
  // deja saine) mais qui doivent, elles aussi, rester intactes.
  int occurrences = 0;
  size_t p = 0;
  while ((p = body.find("kPhGuardReleaseHysteresis", p)) != std::string::npos) {
    occurrences++;
    p += 1;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(6, occurrences,
      "attendu 6 usages de kPhGuardReleaseHysteresis dans le CODE (comments "
      "stripped) : 4 relaches pH + 2 bandes d'hysteresis ORP");
}

void test_t6_code_250_present(void) {
  // Le code de blocage « refus de garde » vit dans constants.h
  // (kDoseBlockedCodeGuard = 250), pas dans pump_controller.*. On verifie a
  // la fois la VALEUR (via le symbole C++ reel, pas recopiee) et sa presence
  // textuelle dans constants.h.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(250, kDoseBlockedCodeGuard,
      "kDoseBlockedCodeGuard n'est plus 250 : le code de blocage a change");

  long len = 0;
  char* src = readProjectFile("src/constants.h", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(src, "src/constants.h introuvable");
  std::string body(src, (size_t)len);
  free(src);
  TEST_ASSERT_TRUE_MESSAGE(
      body.find("constexpr uint8_t kDoseBlockedCodeGuard = 250;") != std::string::npos,
      "la declaration de kDoseBlockedCodeGuard = 250 a disparu du texte source");
}

// =============================================================================
// T7 — les CINQ causes pompe=0 se terminent par un point
// =============================================================================
// Invariant neuf (signale par doc-writer) : l'alerte horaire du mode
// Programme (ph_guard_scheduled, pump_controller.cpp:~1199) concatene la
// cause latchee directement avec " Le volume quotidien...", SANS ajouter son
// propre point final -- elle compte sur le fait que la cause en porte deja
// un. Sans cette assertion, une future cause de garde pH sans point final
// produirait silencieusement « ...suspendu Le volume... » (mots colles).
//
// PORTEE CORRIGEE (revue code-reviewer) : la premiere version de ce test
// excluait a tort le fallback manuel (1178, « regulation pH basculee en
// manuel (refus de garde repetes) ») au motif qu'il serait "jamais concatene
// ainsi". C'etait FAUX : il ecrit lui aussi dans `_lastRefusalCause[0]`, le
// MEME tampon que l'alerte horaire concatene -- via le chemin reel escalade
// en mode `automatic` puis passage en `scheduled` alors que la garde tient
// encore (cause edge-latchee, jamais reecrite). Sans point final, ce site
// produisait silencieusement « ...repetes) Le volume... », le DEFAUT MIROIR
// de celui que ce test existe pour attraper (un point EN TROP donnant
// « ...suspendu.. » ; AUCUN point donnant « ...repetes) Le ... »). D'ou
// CINQ sites verifies, pas quatre : les 4 causes de garde (922, 952, 1022,
// 1059) + le fallback manuel (1178). Seul le filtre `pompe=0` compte
// desormais -- la distinction "porteuse d'une substitution" (utile a T3, qui
// ne regarde QUE les seuils chiffres) n'a plus sa place ici.
//
// Les causes ORP (1324, 1474, 1507), elles, restent HORS PERIMETRE,
// confirme par le relecteur : `pompe=1` alimente `_lastRefusalCause[1]`,
// jamais concatene de cette facon. Y etendre l'invariant serait une
// contrainte sans objet.
void test_t7_les_cinq_causes_pompe0_se_terminent_par_un_point(void) {
  int nbVerifies = 0;
  for (const CallSite& site : g_sites) {
    if (site.name.find("pompe=0") == std::string::npos) continue;
    Analysis a = analyzeCauseExpr(site.exprText);
    char failMsg[320];
    snprintf(failMsg, sizeof(failMsg), "%s : %s", site.name.c_str(), a.failReason.c_str());
    TEST_ASSERT_TRUE_MESSAGE(a.ok, failMsg);
    nbVerifies++;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)a.variantTexts.size(),
        "une cause pompe=0 presente plusieurs variantes (ternaire ?) : inattendu");
    const std::string& text = a.variantTexts[0];
    char msg[320];
    snprintf(msg, sizeof(msg),
        "%s : la cause ne se termine pas par un point -- l'alerte horaire du "
        "mode Programme (ph_guard_scheduled) concatene directement sans en "
        "ajouter un : \"%s\"",
        site.name.c_str(), text.c_str());
    TEST_ASSERT_FALSE_MESSAGE(text.empty(), msg);
    if (!text.empty()) {
      TEST_ASSERT_EQUAL_INT_MESSAGE('.', (int)text.back(), msg);
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, nbVerifies,
      "attendu exactement 5 causes pompe=0 verifiees pour l'invariant du point final "
      "(4 causes de garde + le fallback manuel 1178)");
}

// =============================================================================

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_sentinelle_utf8_strlen_compte_des_octets);

  RUN_TEST(test_recensement_huit_appels_exactement);

  RUN_TEST(test_t1_longueur_apres_substitution_sous_95_octets);

  RUN_TEST(test_t2_aucune_mesure_reintroduite);

  RUN_TEST(test_t3_les_quatre_causes_ph_utilisent_une_decimale);
  RUN_TEST(test_t3_relaches_et_hysteresis_restent_a_deux_decimales);

  RUN_TEST(test_t5_is_rounded_to_tenth_non_vacuite);
  RUN_TEST(test_t5_static_assert_les_quatre_seuils_sont_ronds);

  RUN_TEST(test_t6_discriminance_site_994_1022_if_not_low);
  RUN_TEST(test_t6_inconditionnalite_cause_floor_corrigee);
  RUN_TEST(test_t6_hysteresis_relache_presente);
  RUN_TEST(test_t6_code_250_present);

  RUN_TEST(test_t7_les_cinq_causes_pompe0_se_terminent_par_un_point);

  return UNITY_END();
}
