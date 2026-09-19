// =============================================================================
// Tests unitaires natifs — logger_logic (bug-099)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / LittleFS.
//
// CE QUE CETTE SUITE PROUVE, ET POURQUOI ELLE EST BÂTIE AINSI.
// Les deux chemins de reprise du `.tmp` de rotation ne sont PAS reproductibles :
// la fenêtre `remove`/`rename` de logger.cpp dure quelques millisecondes, et
// aucune route n'écrit de fichier arbitraire sur la partition `history`. La
// preuve repose donc sur trois axes distincts :
//   1. TABLE DE VÉRITÉ du module pur (src/logger_logic.{h,cpp}) — le seul
//      endroit où la décision est prise ;
//   2. SENTINELLES STRUCTURELLES sur le texte de src/logger.cpp — ce que la
//      logique pure ne peut pas prouver seule : que la coquille impure la
//      BRANCHE, dans le BON ORDRE, et ne visse pas le `remove` sur la mauvaise
//      cible. Elles lisent le code DÉPOUILLÉ DE SES COMMENTAIRES (cf.
//      stripCppComments + son auto-test : le 07/09/2026, une sentinelle de ce
//      dépôt a été satisfaite par une simple MENTION EN COMMENTAIRE) ;
//   3. BUDGET de partition, sur le module pur EXISTANT historySaveAdmit() —
//      la conséquence d'origine de la spec, réévaluée sous le layout v5.
//
// ⚠️ L'INVARIANT LE PLUS DANGEREUX EST UN ORDRE, PAS UNE VALEUR. Dans
// setPersistenceFs(), l'ouverture en mode "a" CRÉE /system.log. Réconcilier
// après elle ferait toujours répondre « journal présent » à la sonde : la
// branche Promote deviendrait INATTEIGNABLE et le survivant de la coupure
// partirait en Discard. Ce code-là compile, se relit bien, et DÉTRUIT le
// journal qu'il est censé sauver. C'est test_ordre_* qui l'interdit.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"      // kHistorySaveSpaceMarginBytes
#include "history_logic.h"  // historySaveAdmit (module pur EXISTANT)
#include "logger_logic.h"   // evaluateLogTmpRecovery / classifyLogTmpOutcome

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// AXE 1 — TABLE DE VÉRITÉ (module pur)
// =============================================================================

// -----------------------------------------------------------------------------
// evaluateLogTmpRecovery — les 4 combinaisons de (admitted, systemLogPresent)
// -----------------------------------------------------------------------------

void test_evaluate_non_admis_journal_absent_abstient(void) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)LogTmpRecovery::Abstain,
      (int)evaluateLogTmpRecovery(false, false),
      "Admission refusee : ni promotion ni effacement (critere d'acceptation 3).");
}

void test_evaluate_non_admis_journal_present_abstient_aussi(void) {
  // LE point sensible : sans admission on n'a PAS sondé le FS, donc la valeur
  // de `systemLogPresent` est un artefact. Se replier sur Discard ici
  // effacerait le survivant dans l'etat « journal absent + .tmp present ».
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)LogTmpRecovery::Abstain,
      (int)evaluateLogTmpRecovery(false, true),
      "Une abstention ne se replie JAMAIS vers un effacement.");
}

void test_evaluate_admis_journal_absent_promeut(void) {
  // Etat « absent + present » du tableau de la spec : le .tmp detient l'unique
  // copie des kRotateKeepBytes derniers octets de journal.
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)LogTmpRecovery::Promote,
      (int)evaluateLogTmpRecovery(true, false),
      "Journal absent : le .tmp est le SURVIVANT, il doit etre promu.");
}

void test_evaluate_admis_journal_present_efface(void) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)LogTmpRecovery::Discard,
      (int)evaluateLogTmpRecovery(true, true),
      "Journal courant present : il fait foi, le .tmp est un dechet.");
}

void test_evaluate_ne_rend_JAMAIS_None(void) {
  // `None` est un FAIT (« il n'y avait rien a reconcilier »), jamais une
  // intention : la presence du .tmp n'est pas connue a ce stade. Balayage
  // exhaustif des 4 entrees.
  for (int a = 0; a <= 1; ++a) {
    for (int p = 0; p <= 1; ++p) {
      const LogTmpRecovery r = evaluateLogTmpRecovery(a != 0, p != 0);
      char msg[128];
      snprintf(msg, sizeof(msg),
               "evaluate(%d,%d) rend None : l'intention ne peut pas conclure a "
               "l'absence de .tmp, elle ne l'a pas sondee.", a, p);
      TEST_ASSERT_TRUE_MESSAGE(r != LogTmpRecovery::None, msg);
    }
  }
}

void test_evaluate_ignore_systemLogPresent_quand_non_admis(void) {
  // Formulation directe de l'invariant « parametre ignore » : la sortie ne
  // depend PAS de systemLogPresent des lors que admitted == false.
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)evaluateLogTmpRecovery(false, false),
      (int)evaluateLogTmpRecovery(false, true),
      "systemLogPresent doit etre IGNORE quand l'admission est refusee.");
  // …et l'influence, elle, doit exister quand l'admission est accordee (sans
  // quoi l'assertion ci-dessus serait verte pour une fonction constante).
  TEST_ASSERT_TRUE_MESSAGE(
      evaluateLogTmpRecovery(true, false) != evaluateLogTmpRecovery(true, true),
      "Sous admission, systemLogPresent doit au contraire DECIDER de l'issue.");
}

// -----------------------------------------------------------------------------
// classifyLogTmpOutcome — les 4 intentions x succes/echec
// -----------------------------------------------------------------------------

void test_classify_promote_succes_reste_promote(void) {
  TEST_ASSERT_EQUAL_INT((int)LogTmpRecovery::Promote,
                        (int)classifyLogTmpOutcome(LogTmpRecovery::Promote, true));
}

void test_classify_promote_echec_devient_None(void) {
  // Echec du rename = le .tmp n'existait pas : cas NOMINAL, donc SILENCE.
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)LogTmpRecovery::None,
      (int)classifyLogTmpOutcome(LogTmpRecovery::Promote, false),
      "Un rename echoue ne doit pas faire annoncer une recuperation de journal.");
}

void test_classify_discard_succes_reste_discard(void) {
  TEST_ASSERT_EQUAL_INT((int)LogTmpRecovery::Discard,
                        (int)classifyLogTmpOutcome(LogTmpRecovery::Discard, true));
}

void test_classify_discard_echec_devient_None(void) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)LogTmpRecovery::None,
      (int)classifyLogTmpOutcome(LogTmpRecovery::Discard, false),
      "Cas nominal (aucun .tmp) : aucune ligne de journal au demarrage.");
}

void test_classify_abstain_succes_reste_abstain(void) {
  // Le piege : `true` ne veut RIEN dire pour une abstention (aucun appel FS
  // n'a ete tente). Le laisser promouvoir l'abstention en effet de bord
  // ferait rapporter une action qui n'a pas eu lieu.
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      (int)LogTmpRecovery::Abstain,
      (int)classifyLogTmpOutcome(LogTmpRecovery::Abstain, true),
      "Abstain + succes doit rester Abstain, jamais devenir un effet de bord.");
}

void test_classify_abstain_echec_reste_abstain(void) {
  TEST_ASSERT_EQUAL_INT((int)LogTmpRecovery::Abstain,
                        (int)classifyLogTmpOutcome(LogTmpRecovery::Abstain, false));
}

void test_classify_none_succes_reste_none(void) {
  TEST_ASSERT_EQUAL_INT((int)LogTmpRecovery::None,
                        (int)classifyLogTmpOutcome(LogTmpRecovery::None, true));
}

void test_classify_none_echec_reste_none(void) {
  TEST_ASSERT_EQUAL_INT((int)LogTmpRecovery::None,
                        (int)classifyLogTmpOutcome(LogTmpRecovery::None, false));
}

void test_classify_ne_fabrique_jamais_une_intention_active(void) {
  // Balayage des 4x2 : classify ne peut RENDRE Promote/Discard que si
  // l'intention l'etait deja. Interdit de transformer une abstention ou un
  // silence en action.
  const LogTmpRecovery all[4] = {LogTmpRecovery::None, LogTmpRecovery::Promote,
                                 LogTmpRecovery::Discard, LogTmpRecovery::Abstain};
  for (int i = 0; i < 4; ++i) {
    for (int ok = 0; ok <= 1; ++ok) {
      const LogTmpRecovery out = classifyLogTmpOutcome(all[i], ok != 0);
      const bool actif = (out == LogTmpRecovery::Promote || out == LogTmpRecovery::Discard);
      char msg[160];
      snprintf(msg, sizeof(msg),
               "classify(intent=%d, ok=%d) rend %d : une issue ACTIVE ne peut "
               "sortir que d'une intention active.", (int)all[i], ok, (int)out);
      if (actif) {
        TEST_ASSERT_TRUE_MESSAGE(all[i] == out, msg);
      }
    }
  }
}

// =============================================================================
// AXE 2 — SENTINELLES STRUCTURELLES (texte de src/logger.cpp)
// =============================================================================
// Outillage repris de test/test_native_temp_compensation (readProjectFile,
// stripCppComments, extractFunctionBody) — y compris l'AUTO-TEST du
// depouilleur, sans lequel une sentinelle est satisfiable par un commentaire.

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_logger_logic");
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
    if (len <= 0 || len > 4000000L) { fclose(fp); continue; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(fp); continue; }
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
  while ((p = strstr(p, needle)) != NULL) { ++n; p += len; }
  return n;
}

// Index de la premiere occurrence, ou -1. Sert aux invariants d'ORDRE.
static long indexOf(const char* hay, const char* needle) {
  const char* p = strstr(hay, needle);
  return (p == NULL) ? -1L : (long)(p - hay);
}

static char* extractFunctionBody(const char* src, const char* sig) {
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

// Retire EN PLACE les commentaires C et C++ (remplaces par des espaces,
// longueurs et numeros de ligne preserves). Copie conforme du depouilleur de
// test_native_temp_compensation. MOTIF : le 07/09/2026, une sentinelle de ce
// depot cherchant `clampTempCalOffset(` a ete satisfaite par la seule MENTION
// du nom dans un commentaire de src/config.cpp, et la suite est restee VERTE
// apres suppression de l'appel reel. src/logger.cpp est un cas encore plus
// exposé : ses commentaires citent `reconcileLogTmp()`, `openIfHeapAllows(...,
// "a", ...)`, `remove("/system.log")` et `/history.json` — chacune de ces
// mentions suffirait a fausser une sentinelle naive, dans un sens comme dans
// l'autre.
static void stripCppComments(char* s) {
  bool inLine = false, inBlock = false, inStr = false, inChr = false;
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
    } else if (s[i] == '/' && s[i + 1] == '/') { s[i] = ' '; s[i + 1] = ' '; ++i; inLine = true; }
    else if (s[i] == '/' && s[i + 1] == '*') { s[i] = ' '; s[i + 1] = ' '; ++i; inBlock = true; }
    else if (s[i] == '"') inStr = true;
    else if (s[i] == '\'') inChr = true;
  }
}

// Lit un fichier du depot et en retire les commentaires. TOUTE sentinelle de
// ce fichier passe par ICI, jamais par le texte brut.
static char* readProjectCode(const char* rel) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src != NULL) stripCppComments(src);
  return src;
}

// Compte les SITES D'APPEL de `name` : occurrence de `name(` dont le caractere
// significatif precedent est un separateur d'expression. Ecarte les mentions
// en litteral de chaine et les definitions membres (`Logger::name(`).
static int countCallSites(const char* code, const char* name) {
  char needle[128];
  snprintf(needle, sizeof(needle), "%s(", name);
  const size_t len = strlen(needle);
  int n = 0;
  const char* p = code;
  while ((p = strstr(p, needle)) != NULL) {
    long i = (long)(p - code) - 1;
    while (i >= 0 && (code[i] == ' ' || code[i] == '\n' || code[i] == '\t')) --i;
    const char c = (i < 0) ? ';' : code[i];
    if (c == '=' || c == '(' || c == ',' || c == ';' || c == '{' || c == '}' ||
        c == '&' || c == '|' || c == '!' || c == '?' || c == ':') {
      // `::` (definition membre) est exclu : ':' precede de ':'.
      if (!(c == ':' && i >= 1 && code[i - 1] == ':')) ++n;
    }
    p += len;
  }
  return n;
}

void test_le_depouilleur_de_commentaires_desarme_le_leurre(void) {
  // META-TEST : il verifie l'OUTIL des sentinelles. Sans lui, toutes les
  // assertions de cablage qui suivent pourraient etre vertes pour la mauvaise
  // raison — c'est exactement le defaut releve le 07/09/2026.
  char probe[] =
      "// bug-099 : reconcileLogTmp() en mention de commentaire\n"
      "/* reconcileLogTmp( en bloc */\n"
      "const char* s = \"reconcileLogTmp(\";\n"
      "  reconcileLogTmp();\n";
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, countOccurrences(probe, "reconcileLogTmp("),
      "Le texte BRUT compte les 4 occurrences, dont 3 leurres.");
  stripCppComments(probe);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(probe, "reconcileLogTmp("),
      "Apres depouillement : le litteral de chaine et l'appel reel. Les "
      "commentaires LIGNE et BLOC doivent tous deux disparaitre.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countCallSites(probe, "reconcileLogTmp"),
      "countCallSites doit ignorer la mention en litteral de chaine.");

  // Puis sur le fichier REEL : src/logger.cpp cite `reconcileLogTmp()` dans le
  // commentaire d'ancrage de setPersistenceFs(). Si le depouillement n'en
  // retirait rien, la sentinelle d'ordre serait satisfiable par ce commentaire.
  long len = 0;
  char* raw = readProjectFile("src/logger.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(raw, "src/logger.cpp INTROUVABLE (sentinelle aveugle).");
  const int rawCount = countOccurrences(raw, "reconcileLogTmp");
  stripCppComments(raw);
  const int codeCount = countOccurrences(raw, "reconcileLogTmp");
  char msg[256];
  snprintf(msg, sizeof(msg),
           "src/logger.cpp : %d occurrence(s) brute(s) de reconcileLogTmp contre "
           "%d dans le code depouille. Le depouillement doit en retirer au moins "
           "une (les mentions de commentaire), sinon la sentinelle d'ORDRE reste "
           "satisfiable par un commentaire.", rawCount, codeCount);
  TEST_ASSERT_TRUE_MESSAGE(rawCount > codeCount, msg);
  free(raw);
}

// -----------------------------------------------------------------------------
// B3 — ORDRE dans setPersistenceFs() : l'invariant le plus dangereux
// -----------------------------------------------------------------------------

void test_ordre_reconciliation_avant_ouverture_en_mode_append(void) {
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/logger.cpp INTROUVABLE.");
  char* body = extractFunctionBody(code, "void Logger::setPersistenceFs(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "Logger::setPersistenceFs INTROUVABLE.");

  const long idxReconcile = indexOf(body, "reconcileLogTmp(");
  const long idxAppend = indexOf(body, "\"a\"");
  TEST_ASSERT_TRUE_MESSAGE(idxReconcile >= 0,
      "reconcileLogTmp() n'est PAS appele dans setPersistenceFs() : le .tmp de "
      "rotation n'est jamais reconcilie au demarrage (bug-099 intact).");
  TEST_ASSERT_TRUE_MESSAGE(idxAppend >= 0,
      "Ouverture en mode \"a\" introuvable : sentinelle aveugle, le point de "
      "reference de l'ordre a disparu (revoir ce test).");
  TEST_ASSERT_TRUE_MESSAGE(idxReconcile < idxAppend,
      "ORDRE VIOLE : reconcileLogTmp() est apres l'ouverture en mode \"a\", qui "
      "CREE /system.log. La sonde repondra toujours 'present' : la branche "
      "Promote devient INATTEIGNABLE et le survivant part en Discard — le "
      "journal est detruit par le code cense le sauver.");
  free(body);
  free(code);
}

void test_ordre_reconciliation_avant_flushToDisk(void) {
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL(code);
  char* body = extractFunctionBody(code, "void Logger::setPersistenceFs(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "Logger::setPersistenceFs INTROUVABLE.");

  const long idxReconcile = indexOf(body, "reconcileLogTmp(");
  const long idxFlush = indexOf(body, "flushToDisk(");
  TEST_ASSERT_TRUE_MESSAGE(idxReconcile >= 0, "reconcileLogTmp() absent de setPersistenceFs().");
  TEST_ASSERT_TRUE_MESSAGE(idxFlush >= 0,
      "flushToDisk() introuvable dans setPersistenceFs() : sentinelle aveugle.");
  TEST_ASSERT_TRUE_MESSAGE(idxReconcile < idxFlush,
      "ORDRE VIOLE : flushToDisk() peut relancer une rotation, donc RECREER un "
      ".tmp. Reconcilier apres lui reviendrait a nettoyer une rotation en cours.");
  free(body);
  free(code);
}

void test_ordre_appel_unique_et_dans_la_bonne_fonction(void) {
  // Garde-fou de la sentinelle elle-meme : un appel present DEUX fois, ou
  // deplace dans begin(), invaliderait le raisonnement d'ordre ci-dessus.
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL(code);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countCallSites(code, "reconcileLogTmp"),
      "reconcileLogTmp() doit etre appele EXACTEMENT une fois dans logger.cpp "
      "(hors sa definition membre Logger::reconcileLogTmp).");
  char* body = extractFunctionBody(code, "void Logger::setPersistenceFs(");
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countCallSites(body, "reconcileLogTmp"),
      "L'unique appel doit etre DANS setPersistenceFs().");
  free(body);
  free(code);
}

// -----------------------------------------------------------------------------
// B4 — CIBLE INTERDITE dans reconcileLogTmp()
// -----------------------------------------------------------------------------

void test_la_desambiguation_tmp_vs_journal_est_effective(void) {
  // ⚠️ META-TEST DE LA SENTINELLE SUIVANTE. "/system.log" est un PREFIXE de
  // "/system.log.tmp" : une recherche naive du chemin nu compte les deux, et
  // la sentinelle « aucun remove du journal » devient soit faussement rouge,
  // soit — pire — faussement verte selon le sens de l'assertion. Le motif
  // recherche doit donc inclure le DELIMITEUR de fin (guillemet ou
  // parenthese).
  const char* probe =
      "  fsCallSucceeded = _persistFs->remove(kSystemLogTmpPath);\n"
      "  _persistFs->remove(\"/system.log.tmp\");\n"
      "  File f = openIfHeapAllows(fs, \"/system.log\", \"a\", n);\n";
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(probe, "/system.log"),
      "Recherche NAIVE : 2 trouvailles pour un seul vrai journal — le chemin nu "
      "est aussi trouve DANS celui du .tmp. Une sentinelle batie la-dessus est "
      "faussement rouge (elle croit voir le journal efface).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(probe, "\"/system.log\""),
      "Motif delimite par guillemets : seul le vrai journal est vu.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(probe, "remove(\"/system.log\")"),
      "Et il n'est pas EFFACE ici, seulement ouvert : le motif doit distinguer "
      "l'operation, pas seulement le chemin.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(probe, "remove(kSystemLogPath)"),
      "Motif delimite par parenthese fermante : kSystemLogTmpPath n'est pas pris "
      "pour kSystemLogPath.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(probe, "remove(kSystemLogTmpPath)"),
      "Et le .tmp, lui, doit bien etre vu (sentinelle non aveugle).");
}

void test_reconciliation_ne_supprime_ni_le_journal_ni_l_historique(void) {
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL(code);
  char* body = extractFunctionBody(code, "void Logger::reconcileLogTmp(");
  TEST_ASSERT_NOT_NULL_MESSAGE(body, "Logger::reconcileLogTmp INTROUVABLE.");

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "remove(kSystemLogPath)"),
      "CIBLE INTERDITE : reconcileLogTmp() efface le JOURNAL COURANT. Critere "
      "d'acceptation 6 viole — le nettoyage ne touche jamais /system.log.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "remove(\"/system.log\")"),
      "CIBLE INTERDITE : effacement du journal courant par chemin litteral.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "history.json"),
      "CIBLE INTERDITE : /history.json n'a RIEN a voir avec la rotation du "
      "journal (critere d'acceptation 6).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countOccurrences(body, "kHistoryFile"),
      "CIBLE INTERDITE : aucune reference au fichier d'historique.");

  // Et la cible AUTORISEE doit etre la bonne, exactement une fois.
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, countOccurrences(body, "remove(kSystemLogTmpPath)"),
      "Le seul remove() de la fonction doit viser le .tmp, et lui seul.");
  free(body);
  free(code);
}

void test_promotion_renomme_bien_le_tmp_vers_le_journal(void) {
  // Le sens du rename n'est pas symetrique : inverse, il ECRASE le .tmp avec
  // le journal — ou echoue — et perd le survivant.
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL(code);
  char* body = extractFunctionBody(code, "void Logger::reconcileLogTmp(");
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1,
      countOccurrences(body, "rename(kSystemLogTmpPath, kSystemLogPath)"),
      "La promotion doit renommer tmp -> journal (et non l'inverse).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0,
      countOccurrences(body, "rename(kSystemLogPath, kSystemLogTmpPath)"),
      "SENS INVERSE : le journal ecraserait le .tmp, le survivant serait perdu.");
  free(body);
  free(code);
}

// -----------------------------------------------------------------------------
// B5 — CÂBLAGE : les deux fonctions pures sont vraiment appelees
// -----------------------------------------------------------------------------

void test_cablage_les_deux_fonctions_pures_sont_appelees(void) {
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL(code);
  char* body = extractFunctionBody(code, "void Logger::reconcileLogTmp(");
  TEST_ASSERT_NOT_NULL(body);

  TEST_ASSERT_TRUE_MESSAGE(countCallSites(body, "evaluateLogTmpRecovery") >= 1,
      "evaluateLogTmpRecovery() n'est pas APPELEE dans reconcileLogTmp() : la "
      "decision d'intention est reimplementee ailleurs (ou absente).");
  TEST_ASSERT_TRUE_MESSAGE(countCallSites(body, "classifyLogTmpOutcome") >= 1,
      "classifyLogTmpOutcome() n'est pas APPELEE : le code de retour FS n'est "
      "pas interprete, un cas nominal se mettrait a journaliser.");
  free(body);
  free(code);
}

// ---------------------------------------------------------------------------
// Cardinal de l'enum verrouille — un 5e etat ne peut plus etre avale en silence
// ---------------------------------------------------------------------------
// Les deux `switch` de reconcileLogTmp() traitent None et Abstain par `default:`.
// Un 5e enumerateur y tomberait SANS effet et SANS diagnostic : ni l'env
// materiel ni l'env natif n'activent -Wall, donc -Wswitch est INACTIF, et
// retirer les `default:` ne produirait aucun avertissement du compilateur
// (verifie le 2026-09-08 : platformio.ini ne porte -Wall dans aucun de ses
// trois env). A defaut de diagnostic compilateur, on verrouille le CARDINAL a
// la source : ajouter un etat fait echouer ce test, qui renvoie son auteur vers
// les deux `switch`. Recommandation de la revue du 2026-09-08, option (c).
void test_le_cardinal_de_l_enum_est_verrouille_a_quatre(void) {
  char* code = readProjectCode("src/logger_logic.h");
  TEST_ASSERT_NOT_NULL_MESSAGE(code, "src/logger_logic.h introuvable.");
  char* body = extractFunctionBody(code, "enum class LogTmpRecovery");
  free(code);
  TEST_ASSERT_NOT_NULL_MESSAGE(body,
      "`enum class LogTmpRecovery` introuvable dans src/logger_logic.h DEPOUILLE "
      "de ses commentaires. Si l'enum a ete renomme, adapter cette sentinelle — "
      "ne pas la supprimer.");

  // Un enumerateur par `=` : c'est la forme du source, et le depouillement a
  // retire les commentaires de fin de ligne qui en portaient.
  const int enumerateurs = countOccurrences(body, "=");
  char msg[600];
  snprintf(msg, sizeof(msg),
      "CARDINAL DE L'ENUM CHANGE : %d enumerateurs au lieu de 4. Les deux "
      "`switch` de Logger::reconcileLogTmp() traitent None et Abstain par "
      "`default:` — un etat neuf y tombe SANS EFFET et SANS DIAGNOSTIC (-Wall "
      "n'est active dans aucun env, donc -Wswitch est inactif). Relis ces deux "
      "`switch`, decide explicitement du sort du nouvel etat, puis mets ce test "
      "a jour. Ne le desactive pas : c'est la seule barriere restante.",
      enumerateurs);
  TEST_ASSERT_EQUAL_INT_MESSAGE(4, enumerateurs, msg);

  // Les quatre noms, pour qu'un RENOMMAGE echoue aussi : un cardinal juste avec
  // des noms changes casserait les `switch` sans rien declencher.
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "None") >= 1
                        && countOccurrences(body, "Promote") >= 1
                        && countOccurrences(body, "Discard") >= 1
                        && countOccurrences(body, "Abstain") >= 1,
      "Un des quatre noms (None/Promote/Discard/Abstain) a disparu de l'enum. "
      "Les `switch` de reconcileLogTmp() et les sentinelles de cablage les "
      "visent par nom.");
  free(body);
}

void test_cablage_les_quatre_issues_de_l_enum_sont_traitees(void) {
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL(code);
  char* body = extractFunctionBody(code, "void Logger::reconcileLogTmp(");
  TEST_ASSERT_NOT_NULL(body);

  // Les deux issues ACTIVES sont NOMMEES, et deux fois chacune : une fois pour
  // declencher l'effet de bord (rename / remove), une fois pour emettre SA
  // ligne de journal. Une seule occurrence signifierait qu'un des deux etages
  // a disparu.
  const char* actives[2] = {"LogTmpRecovery::Promote", "LogTmpRecovery::Discard"};
  for (int i = 0; i < 2; ++i) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "%s doit etre nommee DEUX fois dans reconcileLogTmp() (effet de "
             "bord + ligne de journal) ; %d trouvee(s).",
             actives[i], countOccurrences(body, actives[i]));
    TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, actives[i]) >= 2, msg);
  }

  // ⚠️ `None` et `Abstain` ne sont PAS nommees : elles sont traitees par les
  // arms `default:` (silence volontaire). Leur SEMANTIQUE est prouvee par la
  // table de verite du module pur ci-dessus, pas ici. Ce que cette sentinelle
  // exige, c'est qu'un `default:` existe dans CHACUN des deux switch — sans
  // lui, une issue tomberait dans le vide au lieu de se taire.
  // RISQUE RESIDUEL ASSUME : un cinquieme etat ajoute a l'enum serait avale en
  // silence par ces `default:`, et aucun compilateur ne le signalerait.
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(body, "switch ("),
      "reconcileLogTmp() doit garder DEUX switch : l'un sur l'intention (effet "
      "de bord), l'autre sur le FAIT rendu par classifyLogTmpOutcome() "
      "(journalisation). Les fusionner journaliserait un cas nominal.");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countOccurrences(body, "default:"),
      "Chacun des deux switch doit porter un `default:` : c'est la ou None et "
      "Abstain se taisent.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "LogTmpRecovery") >= 5,
      "Le type de decision n'est plus utilise dans reconcileLogTmp() : la "
      "logique pure a ete court-circuitee.");

  // Les DEUX effets de bord, et eux seuls.
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "->rename(") >= 1,
      "Aucun rename() : la promotion du survivant n'a pas lieu.");
  TEST_ASSERT_TRUE_MESSAGE(countOccurrences(body, "->remove(") >= 1,
      "Aucun remove() : le .tmp orphelin n'est jamais efface.");
  free(body);
  free(code);
}

void test_cablage_deux_warnings_distincts_et_aucun_error(void) {
  // Critere d'acceptation 1 : effacement et promotion sont journalises
  // DISTINCTEMENT. Et surtout PAS via error()/critical(), qui flushent en
  // synchrone et rouvriraient /system.log PENDANT la reconciliation.
  char* code = readProjectCode("src/logger.cpp");
  TEST_ASSERT_NOT_NULL(code);
  char* body = extractFunctionBody(code, "void Logger::reconcileLogTmp(");
  TEST_ASSERT_NOT_NULL(body);

  TEST_ASSERT_EQUAL_INT_MESSAGE(2, countCallSites(body, "warning"),
      "Il faut DEUX warning() distincts : un pour la promotion, un pour "
      "l'effacement (ils n'ont pas la meme signification pour le lecteur).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countCallSites(body, "error"),
      "error() flushe en SYNCHRONE : il rouvrirait /system.log pendant la "
      "reconciliation (ADR-0033, anti-flush-storm).");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countCallSites(body, "critical"),
      "critical() flushe en synchrone : meme interdiction.");
  free(body);
  free(code);
}

// =============================================================================
// AXE 3 — BUDGET DE PARTITION (module pur EXISTANT historySaveAdmit)
// =============================================================================
// Ce que la spec bug-099 invoquait comme consequence : un .tmp orphelin de
// 6 144 o mange DEUX blocs LittleFS, definitivement. Les tests ci-dessous
// mesurent la marge AVEC et SANS ce .tmp, aux DEUX tailles de partition :
// celle du layout v5 (256 Ko, ADR-0068) et celle qui a motive la spec (64 Ko).

// Blocs LittleFS consommes par un fichier de `bytes` octets. Le pointeur de
// liste CTZ vit DANS le bloc de donnees (4 a 8 o) : on retient 8, le majorant.
static size_t blocksFor(size_t bytes) {
  const size_t kUsable = 4096 - 8;
  return (bytes + kUsable - 1) / kUsable;
}

// kRotateKeepBytes est LU dans src/logger.h, pas recopie : logger.h n'est pas
// incluable en natif (FS.h / FreeRTOS / std::vector, cf. native_shim).
static size_t rotateKeepBytesFromSource(void) {
  char* code = readProjectCode("src/logger.h");
  if (code == NULL) return 0;
  const char* p = strstr(code, "kRotateKeepBytes");
  if (p == NULL) { free(code); return 0; }
  const char* eq = strchr(p, '=');
  if (eq == NULL) { free(code); return 0; }
  const size_t v = (size_t)strtoul(eq + 1, NULL, 0);
  free(code);
  return v;
}

// Taille de la partition `history` LUE dans partitions.csv (layout v5).
static size_t historyPartitionBytesFromSource(void) {
  long len = 0;
  char* csv = readProjectFile("partitions.csv", &len);
  if (csv == NULL) return 0;
  size_t out = 0;
  const char* p = csv;
  while (p != NULL && *p != '\0') {
    if (*p == '#') {  // commentaire de la table : on saute la ligne
      const char* nl = strchr(p, '\n');
      p = (nl == NULL) ? NULL : nl + 1;
      continue;
    }
    if (strncmp(p, "history", 7) == 0) {
      // history,  data, spiffs,  0x7B0000,0x40000,
      int commas = 0;
      const char* q = p;
      while (*q != '\0' && *q != '\n' && commas < 4) { if (*q == ',') ++commas; ++q; }
      while (*q == ' ') ++q;
      out = (size_t)strtoul(q, NULL, 0);
      break;
    }
    const char* nl = strchr(p, '\n');
    p = (nl == NULL) ? NULL : nl + 1;
  }
  free(csv);
  return out;
}

// feature-131 (v2.53.0) : `historySaveAdmit()` a PERDU son parametre
// `oldFileBytes` — l'ecriture est devenue atomique (.tmp + rename), l'ancien
// /history.json n'est plus supprime avant reecriture, donc son espace n'est
// JAMAIS credite (cf. test_native_history/test_history_logic.cpp, section
// « plus de credit de l'ancien fichier »). Les tests HISTORIQUES de ce fichier
// (regime 64 Ko, motif d'origine de bug-099, remove-avant-ecriture ADR-0033)
// modelisaient explicitement CE credit : ils ne peuvent plus appeler la
// fonction reelle pour rejouer un regime qu'elle n'implemente plus. On
// RECONSTITUE localement l'ANCIENNE formule (jamais reintroduite en
// production) pour ces scenarios strictement historiques ; les scenarios sous
// le layout ACTUEL (v5, 256 Ko) utilisent la VRAIE fonction, avec la VRAIE
// semantique de coexistence (ancien ET nouveau occupent chacun un bloc-arrondi
// de `json` PENDANT toute l'ecriture, cf. constants.h/spec feature-131).
static bool historySaveAdmitLegacyCredit(size_t freeBytes, size_t oldFileBytes,
                                         size_t jsonBytes, size_t marginBytes) {
  size_t freePool = freeBytes, oldPool = oldFileBytes, need = jsonBytes;
  size_t fromFree = (need < freePool) ? need : freePool;
  freePool -= fromFree;
  need -= fromFree;
  if (need > oldPool) return false;
  oldPool -= need;
  need = marginBytes;
  fromFree = (need < freePool) ? need : freePool;
  freePool -= fromFree;
  need -= fromFree;
  return need <= oldPool;
}

// Plus gros /history.json admissible, `.tmp` de rotation du journal present ou
// non — regime HISTORIQUE (64 Ko, remove-avant-ecriture, credit de l'ancien).
// `growthBytes` = ce que le fichier vient de GAGNER (un point RAW ~ 90 o).
static size_t maxAdmissibleJsonLegacyCredit(size_t partitionBytes, size_t tmpBytes,
                                            size_t growthBytes) {
  const size_t horsHisto = (2 + blocksFor(8192) + (tmpBytes ? blocksFor(tmpBytes) : 0)) * 4096;
  size_t best = 0;
  for (size_t json = growthBytes; json <= partitionBytes; json += 1) {
    const size_t occupe = horsHisto + blocksFor(json) * 4096;
    if (occupe > partitionBytes) break;
    const size_t freeBytes = partitionBytes - occupe;
    if (historySaveAdmitLegacyCredit(freeBytes, json - growthBytes, json,
                                     kHistorySaveSpaceMarginBytes)) {
      best = json;
    }
  }
  return best;
}

// Plus gros /history.json admissible — regime ACTUEL (feature-131, .tmp +
// rename) : l'ancien ET le nouveau fichier occupent CHACUN un bloc-arrondi de
// `json` pendant toute l'ecriture (coexistence, AUCUN credit).
static size_t maxAdmissibleJsonCoexistence(size_t partitionBytes, size_t tmpBytes) {
  const size_t horsHisto = (2 + blocksFor(8192) + (tmpBytes ? blocksFor(tmpBytes) : 0)) * 4096;
  size_t best = 0;
  for (size_t json = 1; json <= partitionBytes; json += 1) {
    const size_t occupe = horsHisto + 2 * blocksFor(json) * 4096;
    if (occupe > partitionBytes) break;
    const size_t freeBytes = partitionBytes - occupe;
    if (historySaveAdmit(freeBytes, json, kHistorySaveSpaceMarginBytes)) {
      best = json;
    }
  }
  return best;
}

void test_budget_les_constantes_sont_lues_a_la_source(void) {
  // Sentinelle des tests de budget eux-memes : s'ils raisonnaient sur des
  // chiffres recopies, ils resteraient verts apres un changement de layout.
  TEST_ASSERT_EQUAL_size_t_MESSAGE(6144, rotateKeepBytesFromSource(),
      "kRotateKeepBytes a change dans src/logger.h : tout le chiffrage de "
      "budget ci-dessous porte sur 6 144 o de .tmp, a reprendre.");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(262144, historyPartitionBytesFromSource(),
      "La partition `history` de partitions.csv ne fait plus 256 Ko (layout v5, "
      "ADR-0068) : le verdict 'consequence dissoute' est a refaire.");
}

void test_budget_le_tmp_orphelin_coute_exactement_deux_blocs(void) {
  const size_t tmp = rotateKeepBytesFromSource();
  TEST_ASSERT_EQUAL_size_t_MESSAGE(2, blocksFor(tmp),
      "6 144 o traversent une frontiere de bloc : le .tmp orphelin immobilise "
      "8 192 o, pas 6 144.");
}

void test_budget_64Ko_le_tmp_orphelin_etait_la_difference(void) {
  // Les chiffres HISTORIQUES qui ont motive la spec (partition de 64 Ko,
  // regime remove-avant-ecriture ADR-0033 — reconstitue localement, feature-131
  // a retire cette semantique de historySaveAdmit()).
  const size_t p64 = 65536;
  const size_t tmp = rotateKeepBytesFromSource();
  const size_t sansTmp = maxAdmissibleJsonLegacyCredit(p64, 0, 90);
  const size_t avecTmp = maxAdmissibleJsonLegacyCredit(p64, tmp, 90);
  char msg[256];
  snprintf(msg, sizeof(msg),
           "64 Ko : /history.json admissible au plus a %zu o sans .tmp, %zu o "
           "avec — perte %zu o.", sansTmp, avecTmp, sansTmp - avecTmp);
  TEST_ASSERT_TRUE_MESSAGE(avecTmp < sansTmp, msg);
  // La perte vaut les deux blocs immobilises, a l'arrondi bloc du fichier pres.
  TEST_ASSERT_TRUE_MESSAGE(sansTmp - avecTmp >= 4096, msg);
  // Et surtout : sous 64 Ko, le fichier reel de v2.38 (7 blocs, ~25,5 Ko) se
  // situait DANS la bande perdue. C'est ce que mesure le test suivant.
}

void test_budget_64Ko_la_bande_perdue_contenait_le_fichier_reel(void) {
  // Bande realiste du /history.json d'aujourd'hui : 20 000 a 31 000 o (les
  // trois variantes chiffrees par test_native_history valent 23 497 / 25 513 /
  // 30 616 o — on balaye la bande ENTIERE plutot que de recopier un point, le
  // modele de taille etant deja signale comme perime par bug-098).
  const size_t p64 = 65536;
  const size_t tmp = rotateKeepBytesFromSource();
  const size_t horsHistoAvec = (2 + blocksFor(8192) + blocksFor(tmp)) * 4096;
  const size_t horsHistoSans = (2 + blocksFor(8192)) * 4096;
  int refusesAvecTmp = 0, refusesSansTmp = 0;
  for (size_t json = 20000; json <= 31000; json += 89) {
    const size_t occA = horsHistoAvec + blocksFor(json) * 4096;
    const bool admisA = (occA <= p64) &&
        historySaveAdmitLegacyCredit(p64 - occA, json - 90, json, kHistorySaveSpaceMarginBytes);
    const size_t occS = horsHistoSans + blocksFor(json) * 4096;
    const bool admisS = (occS <= p64) &&
        historySaveAdmitLegacyCredit(p64 - occS, json - 90, json, kHistorySaveSpaceMarginBytes);
    if (!admisA) ++refusesAvecTmp;
    if (!admisS) ++refusesSansTmp;
  }
  char msg[256];
  snprintf(msg, sizeof(msg),
           "Bande 20-31 Ko sous 64 Ko : %d refus avec le .tmp orphelin, %d sans. "
           "Le .tmp doit AGGRAVER le refus (motif d'origine de bug-099).",
           refusesAvecTmp, refusesSansTmp);
  TEST_ASSERT_TRUE_MESSAGE(refusesAvecTmp > refusesSansTmp, msg);
  TEST_ASSERT_TRUE_MESSAGE(refusesAvecTmp > 0,
      "Sous 64 Ko avec .tmp orphelin, au moins une taille realiste de "
      "/history.json doit etre refusee — sinon la spec n'avait pas de motif.");
}

void test_budget_v5_la_consequence_est_dissoute_pas_la_cause(void) {
  // ⚠️ VERDICT REEL, a ne pas lire comme un echec du lot : sous le layout v5
  // (256 Ko), les deux blocs du .tmp orphelin ne refusent AUCUNE taille
  // realiste de /history.json. La consequence budgetaire d'origine est bien
  // DISSOUTE. Ce que le lot corrige est autre chose : la PERTE DE JOURNAL dans
  // l'etat « log absent + tmp present » (axes 1 et 2 ci-dessus).
  const size_t p = historyPartitionBytesFromSource();
  TEST_ASSERT_TRUE_MESSAGE(p >= 262144, "Partition history < 256 Ko : verdict a refaire.");
  const size_t tmp = rotateKeepBytesFromSource();
  // feature-131 : regime REEL depuis ce lot — coexistence ancien+nouveau
  // (.tmp+rename), AUCUN credit. L'ancien /history.json ET le .tmp du nouveau
  // occupent CHACUN un bloc-arrondi de `json` pendant toute l'ecriture — c'est
  // STRICTEMENT PLUS exigeant que le regime credite d'avant ce lot, et le
  // verdict « consequence dissoute » doit tenir MEME sous cette exigence accrue.
  const size_t horsHisto = (2 + blocksFor(8192) + blocksFor(tmp)) * 4096;
  for (size_t json = 20000; json <= 31000; json += 89) {
    const size_t occ = horsHisto + 2 * blocksFor(json) * 4096;
    char msg[192];
    snprintf(msg, sizeof(msg),
             "Sous v5 (%zu o) avec coexistence ancien+nouveau (feature-131), "
             "/history.json de %zu o refuse malgre un .tmp orphelin : la "
             "consequence budgetaire ne serait PAS dissoute.", p, json);
    TEST_ASSERT_TRUE_MESSAGE(occ <= p, msg);
    TEST_ASSERT_TRUE_MESSAGE(
        historySaveAdmit(p - occ, json, kHistorySaveSpaceMarginBytes), msg);
  }
  // Marge restante : de quoi encaisser bien plus que les 8 192 o du .tmp.
  const size_t sansTmp = maxAdmissibleJsonCoexistence(p, 0);
  const size_t avecTmp = maxAdmissibleJsonCoexistence(p, tmp);
  char msg[256];
  snprintf(msg, sizeof(msg),
           "v5 (256 Ko) : /history.json admissible jusqu'a %zu o sans .tmp et "
           "%zu o avec. Le plafond reste tres au-dessus des ~31 Ko du fichier "
           "reel : six kilooctets orphelins ne menacent plus le budget.",
           sansTmp, avecTmp);
  TEST_ASSERT_TRUE_MESSAGE(avecTmp > 31000 * 2, msg);
  TEST_MESSAGE(msg);
}

// =============================================================================

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  UNITY_BEGIN();

  // B1 — table de verite : evaluateLogTmpRecovery
  RUN_TEST(test_evaluate_non_admis_journal_absent_abstient);
  RUN_TEST(test_evaluate_non_admis_journal_present_abstient_aussi);
  RUN_TEST(test_evaluate_admis_journal_absent_promeut);
  RUN_TEST(test_evaluate_admis_journal_present_efface);
  RUN_TEST(test_evaluate_ne_rend_JAMAIS_None);
  RUN_TEST(test_evaluate_ignore_systemLogPresent_quand_non_admis);

  // B1 — table de verite : classifyLogTmpOutcome
  RUN_TEST(test_classify_promote_succes_reste_promote);
  RUN_TEST(test_classify_promote_echec_devient_None);
  RUN_TEST(test_classify_discard_succes_reste_discard);
  RUN_TEST(test_classify_discard_echec_devient_None);
  RUN_TEST(test_classify_abstain_succes_reste_abstain);
  RUN_TEST(test_classify_abstain_echec_reste_abstain);
  RUN_TEST(test_classify_none_succes_reste_none);
  RUN_TEST(test_classify_none_echec_reste_none);
  RUN_TEST(test_classify_ne_fabrique_jamais_une_intention_active);

  // B2 — auto-test du depouilleur de commentaires (anti-leurre)
  RUN_TEST(test_le_depouilleur_de_commentaires_desarme_le_leurre);

  // B3 — ORDRE dans setPersistenceFs()
  RUN_TEST(test_ordre_reconciliation_avant_ouverture_en_mode_append);
  RUN_TEST(test_ordre_reconciliation_avant_flushToDisk);
  RUN_TEST(test_ordre_appel_unique_et_dans_la_bonne_fonction);

  // B4 — cible interdite
  RUN_TEST(test_la_desambiguation_tmp_vs_journal_est_effective);
  RUN_TEST(test_reconciliation_ne_supprime_ni_le_journal_ni_l_historique);
  RUN_TEST(test_promotion_renomme_bien_le_tmp_vers_le_journal);

  // B5 — cablage
  RUN_TEST(test_cablage_les_deux_fonctions_pures_sont_appelees);
  RUN_TEST(test_cablage_les_quatre_issues_de_l_enum_sont_traitees);
  RUN_TEST(test_le_cardinal_de_l_enum_est_verrouille_a_quatre);
  RUN_TEST(test_cablage_deux_warnings_distincts_et_aucun_error);

  // B6 — budget de partition (historySaveAdmit)
  RUN_TEST(test_budget_les_constantes_sont_lues_a_la_source);
  RUN_TEST(test_budget_le_tmp_orphelin_coute_exactement_deux_blocs);
  RUN_TEST(test_budget_64Ko_le_tmp_orphelin_etait_la_difference);
  RUN_TEST(test_budget_64Ko_la_bande_perdue_contenait_le_fichier_reel);
  RUN_TEST(test_budget_v5_la_consequence_est_dissoute_pas_la_cause);

  return UNITY_END();
}
