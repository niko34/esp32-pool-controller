// =============================================================================
// Tests natifs — feature-148 : UN MESSAGE, UN SEUL ENDROIT (silence du badge)
// =============================================================================
// CE QUE CE LOT PROTÈGE
// --------------------
// Le badge de carte (pH / ORP, tableau de bord) ne doit pas répéter une cause
// déjà portée par le bandeau global (#dose-block-banner). Les FAITS suivants
// sont extraits du TEXTE de data/app.js (aucun harnais JS dans ce dépôt —
// patron imposé, identique à test_native_dose_mask / test_native_ws_data_parity) :
//   F1 — DOSE_BLOCK_CODE_ALREADY_SOURCED (le Set historique, « déjà sourcé
//        ailleurs DANS le badge »)
//   F2 — le SENS de la garde ajoutée en dosingLimitSegments() : la condition
//        `doseBlockBannerCarries(sensor, data)` est-elle préfixée d'un `!` ?
//        (kGuardNegated)
//   F3 — DOSE_BLOCK_MEASURE_WAIT_CODES (condition de fusion cause-racine
//        pH → ORP)
//   F4 — revue feature-148 (relecteur) : les QUATRE littéraux `carries: {...}`
//        de `_doseBlockBannerPlan()`, un par branche (vide/vide, fusion
//        cause-racine, fusion même-code, lignes indépendantes), extraits
//        TELS QUELS (« true »/« false »/« !!ph »/« !!orp ») et évalués par le
//        miroir — jamais réimplémentés en dur. C'est ce qui manquait à la
//        première passe : muter `{ ph: !!ph, orp: !!orp }` en
//        `{ ph: true, orp: true }` dans data/app.js ne changeait RIEN à un
//        miroir qui recalculait la même chose en C++ indépendamment du texte.
//   F6 — RETIRÉ (feature-152, seconde passe, 2026-10-11) : la garde
//        `FilterAlarm` de `_doseBlockRowData()` protégeait `phFilterAlarm`/
//        `orpFilterAlarm` et `#regul-alert`, des champs/éléments qui ONT
//        DISPARU avec le mécanisme de filtre qu'ils signalaient (plus de
//        verrou collant côté firmware ⇒ plus rien à alarmer côté interface).
//        Les trois tests qui en dépendaient (F6 lui-même, et les deux
//        scénarios FilterAlarm de la face B) sont RETIRÉS avec elle — un
//        mécanisme qui n'existe plus n'a pas de comportement à verrouiller.
//        La garde anti-retour du vocabulaire (D6/D7, tools/check_removed_
//        measurement_vocabulary.sh) couvre désormais la non-réapparition de
//        « FilterAlarm »/« regul-alert » dans data/.
//
// Ces faits alimentent un MIROIR pur (bannerPlan + badgePushesBlockLabel) qui
// REJOUE le calcul de `_doseBlockBannerPlan()` / la garde de
// `dosingLimitSegments()` à partir de littéraux LUS, pas recopiés dans la
// logique C++. La branche-sélection (quelles conditions déclenchent quelle
// fusion) reste, elle, réimplémentée — seuls les DEUX points que le relecteur
// a prouvés aveugles (F4, F6) sont désormais des faits extraits.
//
// F7 — invariant d'ORDRE ajouté en revue (silence-sans-porteur temporaire,
//      cf. commentaire data/app.js:7453) : l'unique appel à
//      `updateDoseBlockBanner(json)` doit précéder, dans le même bloc, le
//      prochain appel à `updateSensorBadges()`. Vérifié littéralement.
//
// LIMITE CONNUE (à ne pas masquer) : cette suite ne prouve PAS que
// `_doseBlockBannerPlan()` est fidèle au DOM réellement rendu — seul un
// refactor qui dériverait le rendu du plan sans aucune décision résiduelle,
// plus un test manuel sur cible, fermerait ce doute. Voir le plan de test
// manuel de la spec feature-148.
// =============================================================================

#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Lecture des sources du projet (patron test_native_dose_mask)
// -----------------------------------------------------------------------------

static void stripComments(char* s);

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  const char* candidates[2];

  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_dose_banner_silence/");
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

// Neutralise les commentaires (remplacés par des espaces), conserve le contenu
// des littéraux de chaîne ('...'/"...") : les motifs recherchés sont du CODE,
// jamais une citation en commentaire (bug-098 avait le même piège).
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

static int countIn(const char* hay, const char* needle) {
  int c = 0;
  const size_t n = strlen(needle);
  const size_t len = strlen(hay);
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(hay + i, needle, n) == 0) c++;
  }
  return c;
}

// =============================================================================
// EXTRACTION DES TROIS FAITS
// =============================================================================

static int parseIntList(const char* s, int* out, int maxN) {
  int n = 0;
  const char* p = s;
  while (*p != '\0' && *p != ']') {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (*p == ']' || *p == '\0') break;
    int v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (any && n < maxN) out[n++] = v;
    if (!any) p++;  // caractère inattendu : on avance pour ne pas boucler
  }
  return n;
}

static int extractListAfter(const char* text, const char* anchor, int* out, int maxN) {
  const char* a = strstr(text, anchor);
  if (a == NULL) return -1;
  return parseIntList(a + strlen(anchor), out, maxN);
}

// F2 — localise `doseBlockBannerCarries(` DANS LE CORPS de dosingLimitSegments()
// (pas sa définition, plus bas dans le fichier) et dit si l'appel est précédé
// d'un `!`. Retourne 1 (négué), 0 (non négué), -1 (introuvable — échec dur).
static int extractGuardNegated(const char* text) {
  const char* fnStart = strstr(text, "function dosingLimitSegments(");
  if (fnStart == NULL) return -1;
  const char* fnEnd = strstr(fnStart, "function buildSensorSegments(");
  if (fnEnd == NULL) fnEnd = fnStart + strlen(fnStart);

  const char* anchor = "doseBlockBannerCarries(";
  const size_t an = strlen(anchor);
  for (const char* q = fnStart; q + an <= fnEnd; ++q) {
    if (memcmp(q, anchor, an) == 0) {
      const char* b = q - 1;
      while (b > fnStart && *b == ' ') b--;
      return (*b == '!') ? 1 : 0;
    }
  }
  return -1;
}

// F4 (revue feature-148) — extrait le littéral `carries: { ph: X, orp: Y }`
// qui suit immédiatement `searchFrom` (recherche bornée, pas plein texte :
// deux branches de fusion partagent le MÊME littéral « true, true », seul
// leur ANCRE de recherche les distingue). X/Y sont recopiés TELS QUELS
// ("true" / "false" / "!!ph" / "!!orp") — jamais interprétés ici : c'est
// `evalCarriesToken()` qui leur donne un sens, et qui échoue fort si la forme
// n'est plus l'une des quatre attendues (un littéral qui change de FORME doit
// être vu, pas silencieusement toléré).
static int parseCarriesLiteral(const char* searchFrom, char* phTok, size_t phCap,
                                char* orpTok, size_t orpCap) {
  const char* c = strstr(searchFrom, "carries:");
  if (c == NULL) return 0;
  const char* open = strchr(c, '{');
  if (open == NULL) return 0;
  const char* close = strchr(open, '}');
  if (close == NULL) return 0;
  const char* phk = strstr(open, "ph:");
  const char* orpk = strstr(open, "orp:");
  if (phk == NULL || orpk == NULL || phk > close || orpk > close) return 0;

  const char* phStart = phk + 3;
  while (*phStart == ' ') phStart++;
  const char* phEnd = strchr(phStart, ',');
  if (phEnd == NULL || phEnd > close) phEnd = close;
  size_t phLen = (size_t)(phEnd - phStart);
  while (phLen > 0 && phStart[phLen - 1] == ' ') phLen--;
  if (phLen == 0 || phLen >= phCap) return 0;
  memcpy(phTok, phStart, phLen);
  phTok[phLen] = '\0';

  const char* orpStart = orpk + 4;
  while (*orpStart == ' ') orpStart++;
  size_t orpLen = (size_t)(close - orpStart);
  while (orpLen > 0 && orpStart[orpLen - 1] == ' ') orpLen--;
  if (orpLen == 0 || orpLen >= orpCap) return 0;
  memcpy(orpTok, orpStart, orpLen);
  orpTok[orpLen] = '\0';
  return 1;
}

// -----------------------------------------------------------------------------
// État global de la suite — chargé une fois, revérifié à chaque test
// -----------------------------------------------------------------------------
static char* g_app = NULL;
static int g_f1[32];
static int g_f1Count = -2;   // -2 = pas encore chargé
static int g_f3[16];
static int g_f3Count = -2;
static int g_guardNegated = -2;

// F4 — un couple de jetons par branche de _doseBlockBannerPlan().
static char g_tokEmptyPh[8], g_tokEmptyOrp[8];
static char g_tokFusionRootPh[8], g_tokFusionRootOrp[8];
static char g_tokFusionSamePh[8], g_tokFusionSameOrp[8];
static char g_tokIndepPh[8], g_tokIndepOrp[8];
static int g_f4Ok = -2;

static void ensureLoaded(void) {
  if (g_app != NULL) return;
  long len = 0;
  g_app = readProjectFile("data/app.js", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(g_app, "data/app.js introuvable depuis le test natif");
  if (g_app == NULL) return;

  g_f1Count = extractListAfter(g_app, "DOSE_BLOCK_CODE_ALREADY_SOURCED = new Set([", g_f1, 32);
  g_f3Count = extractListAfter(g_app, "DOSE_BLOCK_MEASURE_WAIT_CODES = [", g_f3, 16);
  g_guardNegated = extractGuardNegated(g_app);

  TEST_ASSERT_TRUE_MESSAGE(g_f1Count > 0,
    "F1 introuvable : DOSE_BLOCK_CODE_ALREADY_SOURCED a disparu ou changé de forme dans data/app.js");
  TEST_ASSERT_TRUE_MESSAGE(g_f3Count > 0,
    "F3 introuvable : DOSE_BLOCK_MEASURE_WAIT_CODES a disparu ou changé de forme dans data/app.js");
  TEST_ASSERT_TRUE_MESSAGE(g_guardNegated >= 0,
    "F2 introuvable : l'appel doseBlockBannerCarries(sensor, data) a disparu de dosingLimitSegments()");

  // F4 — ancres BORNÉES, une par branche (voir commentaire de parseCarriesLiteral).
  const char* anchorEmpty = strstr(g_app,
    "visible: false, rowPh: null, rowOrp: null, carries:");
  const char* anchorFusionRoot = strstr(g_app, "arrive jusqu'aux sondes.");
  const char* anchorFusionSame = strstr(g_app, "(pH et chlore)`,");
  // Ancre STABLE, distincte du littéral muté : du CODE (les commentaires sont
  // effacés par stripComments avant la recherche), qui précède le littéral
  // sans en faire partie. Une ancre qui EST le texte muté ne détecterait
  // jamais la mutation : elle disparaîtrait avec lui, et parseCarriesLiteral
  // échouerait "proprement" au lieu de lire la valeur mutée.
  const char* anchorIndep = strstr(g_app, "shortcutCode: canShortcut(orp) ? orp.code : null,");

  g_f4Ok = 1;
  if (anchorEmpty == NULL ||
      !parseCarriesLiteral(anchorEmpty, g_tokEmptyPh, sizeof(g_tokEmptyPh),
                            g_tokEmptyOrp, sizeof(g_tokEmptyOrp))) g_f4Ok = 0;
  if (anchorFusionRoot == NULL ||
      !parseCarriesLiteral(anchorFusionRoot, g_tokFusionRootPh, sizeof(g_tokFusionRootPh),
                            g_tokFusionRootOrp, sizeof(g_tokFusionRootOrp))) g_f4Ok = 0;
  if (anchorFusionSame == NULL ||
      !parseCarriesLiteral(anchorFusionSame, g_tokFusionSamePh, sizeof(g_tokFusionSamePh),
                            g_tokFusionSameOrp, sizeof(g_tokFusionSameOrp))) g_f4Ok = 0;
  if (anchorIndep == NULL ||
      !parseCarriesLiteral(anchorIndep, g_tokIndepPh, sizeof(g_tokIndepPh),
                            g_tokIndepOrp, sizeof(g_tokIndepOrp))) g_f4Ok = 0;

  TEST_ASSERT_TRUE_MESSAGE(g_f4Ok,
    "F4 introuvable : un littéral carries:{...} d'une des 4 branches de "
    "_doseBlockBannerPlan() a disparu ou changé de forme dans data/app.js");

  // F6 (garde FilterAlarm) retiré avec le mécanisme qu'il protégeait — voir
  // le commentaire d'en-tête. Verrou de non-retour : la garde doit rester
  // ABSENTE (sa réapparition romprait l'invariant « un seul endroit par
  // mécanisme », couvert par ailleurs par la garde anti-retour D6/D7).
  TEST_ASSERT_NULL_MESSAGE(strstr(g_app, "'FilterAlarm'] === true) return null;"),
    "la garde FilterAlarm est de retour dans _doseBlockRowData() alors que le "
    "mecanisme qu'elle protegeait (phFilterAlarm/orpFilterAlarm) a disparu — "
    "verifier qu'il ne s'agit pas d'une reintroduction du filtre non pret");
}

static int inList(const int* lst, int n, int v) {
  for (int i = 0; i < n; ++i) if (lst[i] == v) return 1;
  return 0;
}

// =============================================================================
// MIROIR — rejoue le comportement à partir des faits extraits (F1-F4, F6).
// La sélection de branche (quelle condition déclenche quelle fusion) reste
// réimplémentée ; seule la SORTIE `carries` de chaque branche, et la garde
// FilterAlarm, sont désormais des faits LUS plutôt que recopiés en dur — ce
// sont précisément les deux points que la revue feature-148 a prouvés
// aveugles (muter le littéral indépendant en {true,true}, ou supprimer la
// garde FilterAlarm, laissait les 18 cas de la passe précédente intacts).
// =============================================================================

struct RowIn {
  int present;         // la voie existe-t-elle dans latestSensorData ?
  int code;
  const char* tone;    // "none" | "fault" | "normal" | "unknown" | "warning"
};

struct Carries { int ph; int orp; };

// Interprète un jeton F4 ("true"/"false"/"!!ph"/"!!orp"). Échec dur si la
// forme a changé : un jeton qu'on ne reconnaît plus ne doit jamais retomber
// silencieusement sur une valeur par défaut plausible.
static int evalCarriesToken(const char* tok, int phRow, int orpRow) {
  if (strcmp(tok, "true") == 0) return 1;
  if (strcmp(tok, "false") == 0) return 0;
  if (strcmp(tok, "!!ph") == 0) return phRow;
  if (strcmp(tok, "!!orp") == 0) return orpRow;
  char msg[160];
  snprintf(msg, sizeof(msg), "jeton carries inattendu (%s) — forme du littéral changée dans data/app.js", tok);
  TEST_FAIL_MESSAGE(msg);
  return 0;
}

// Mirroir de _doseBlockRowData() : null si tone === 'none'. (F6, la garde
// FilterAlarm, est retirée avec le mécanisme qu'elle protégeait — voir
// l'en-tête du fichier.)
static int rowExists(const RowIn& r) {
  return r.present && strcmp(r.tone, "none") != 0;
}

// Mirroir de _doseBlockBannerPlan() — la sortie `carries` de chaque branche
// est désormais ÉVALUÉE depuis le jeton F4 lu en source, pas recopiée en dur.
static Carries bannerPlan(const RowIn& ph, const RowIn& orp) {
  const int phRow = rowExists(ph);
  const int orpRow = rowExists(orp);
  if (!phRow && !orpRow) {
    Carries c = { evalCarriesToken(g_tokEmptyPh, phRow, orpRow),
                  evalCarriesToken(g_tokEmptyOrp, phRow, orpRow) };
    return c;
  }

  // Fusion cause-racine pH → ORP : orp.code === 250 ET ph.code ∈ F3.
  if (phRow && orpRow && orp.code == 250 && inList(g_f3, g_f3Count, ph.code)) {
    Carries c = { evalCarriesToken(g_tokFusionRootPh, phRow, orpRow),
                  evalCarriesToken(g_tokFusionRootOrp, phRow, orpRow) };
    return c;
  }
  // Fusion même-code, ton normal des deux côtés.
  if (phRow && orpRow && ph.code == orp.code &&
      strcmp(ph.tone, "normal") == 0 && strcmp(orp.tone, "normal") == 0) {
    Carries c = { evalCarriesToken(g_tokFusionSamePh, phRow, orpRow),
                  evalCarriesToken(g_tokFusionSameOrp, phRow, orpRow) };
    return c;
  }
  Carries c = { evalCarriesToken(g_tokIndepPh, phRow, orpRow),
                evalCarriesToken(g_tokIndepOrp, phRow, orpRow) };
  return c;
}

// Mirroir du garde-fou de dosingLimitSegments() (ligne ~3048) :
//   block.label && !F1.has(block.code) && <guard>(carries)
// où <guard> vaut `!carries` si kGuardNegated, `carries` sinon.
static int badgePushesBlockLabel(const char* label, int code, int carriesForSensor) {
  if (label == NULL || label[0] == '\0') return 0;
  if (inList(g_f1, g_f1Count, code)) return 0;
  const int guard = g_guardNegated ? !carriesForSensor : carriesForSensor;
  return guard;
}

// =============================================================================
// FACE A — les trois faits extraits sont bien ceux attendus
// =============================================================================

static void test_f1_contenu_attendu(void) {
  ensureLoaded();
  // feature-152 (seconde passe) : 4, 19, 22 retirés (jamais recyclés) — la
  // garde séparée « filtre non prêt »/qualifications associées a disparu.
  const int expected[] = {2, 3, 7, 8, 10, 11, 12, 15, 18};
  const int n = (int)(sizeof(expected) / sizeof(expected[0]));
  char msg[200];
  snprintf(msg, sizeof(msg), "F1 attendu %d codes, lu %d", n, g_f1Count);
  TEST_ASSERT_EQUAL_INT_MESSAGE(n, g_f1Count, msg);
  for (int i = 0; i < n; ++i) {
    snprintf(msg, sizeof(msg), "F1 doit contenir le code %d (déjà sourcé ailleurs)", expected[i]);
    TEST_ASSERT_TRUE_MESSAGE(inList(g_f1, g_f1Count, expected[i]), msg);
  }
}

static void test_f3_contenu_attendu(void) {
  ensureLoaded();
  // feature-152 (seconde passe) : réduite de SIX à TROIS codes — 4/19/22
  // retirés, l'ORP partage désormais 15/18 avec le pH (plus de qualification
  // séparée « garde filtre non prêt »).
  const int expected[] = {3, 15, 18};
  const int n = (int)(sizeof(expected) / sizeof(expected[0]));
  char msg[220];
  snprintf(msg, sizeof(msg), "F3 attendu %d codes, lu %d — une liste rétrécie (ex. [3]) "
           "romprait la fusion pH->ORP sur 15/18 sans le dire", n, g_f3Count);
  TEST_ASSERT_EQUAL_INT_MESSAGE(n, g_f3Count, msg);
  for (int i = 0; i < n; ++i) {
    snprintf(msg, sizeof(msg), "F3 doit contenir le code %d (attente de mesure, fusion)", expected[i]);
    TEST_ASSERT_TRUE_MESSAGE(inList(g_f3, g_f3Count, expected[i]), msg);
  }
}

static void test_f2_garde_est_negatee(void) {
  ensureLoaded();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_guardNegated,
    "dosingLimitSegments() doit pousser le libellé générique quand la voie "
    "N'EST PAS portée par le bandeau : la condition doit lire "
    "`!doseBlockBannerCarries(sensor, data)`, pas sa forme non négatée.");
}

// Revue feature-148 (relecteur) — F4 : les QUATRE littéraux `carries:` lus
// doivent valoir EXACTEMENT ce que chaque branche affirme. Un test qui se
// contenterait d'un `g_f4Ok` structurel ne tuerait rien sur un contenu
// changé mais toujours bien formé (ex. `{ ph: true, orp: false }` au lieu de
// `{ ph: false, orp: false }` sur la branche vide) — on compare donc chaîne à
// chaîne.
static void test_f4_contenu_attendu(void) {
  ensureLoaded();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("false", g_tokEmptyPh, "branche vide/vide, ph");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("false", g_tokEmptyOrp, "branche vide/vide, orp");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("true", g_tokFusionRootPh, "branche fusion cause-racine, ph");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("true", g_tokFusionRootOrp, "branche fusion cause-racine, orp");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("true", g_tokFusionSamePh, "branche fusion même-code, ph");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("true", g_tokFusionSameOrp, "branche fusion même-code, orp");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("!!ph", g_tokIndepPh, "branche lignes indépendantes, ph");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("!!orp", g_tokIndepOrp, "branche lignes indépendantes, orp");
}

// Revue feature-148 (point de robustesse signalé par le code-reviewer,
// commentaire data/app.js:7453) — F7 : updateDoseBlockBanner(json) doit
// s'exécuter AVANT updateSensorBadges(), dans le MÊME bloc d'appel, sinon
// une exception intermédiaire peut figer le bandeau sur une trame périmée
// pendant que le ticker continue de taire le badge sur la trame courante —
// un silence SANS porteur.
static void test_f7_bandeau_mis_a_jour_avant_les_badges(void) {
  ensureLoaded();
  const char* callSite = strstr(g_app, "updateDoseBlockBanner(json);");
  TEST_ASSERT_NOT_NULL_MESSAGE(callSite,
    "l'appel à updateDoseBlockBanner(json) a disparu de data/app.js");
  if (callSite == NULL) return;
  const char* nextBadgeCall = strstr(callSite, "updateSensorBadges();");
  TEST_ASSERT_NOT_NULL_MESSAGE(nextBadgeCall,
    "aucun appel à updateSensorBadges() ne suit plus updateDoseBlockBanner(json) — "
    "l'ordre a été inversé (cf. data/app.js:7453) ou les deux appels ont été séparés");
  if (nextBadgeCall == NULL) return;
  const ptrdiff_t gap = nextBadgeCall - callSite;
  char msg[200];
  snprintf(msg, sizeof(msg),
    "updateSensorBadges() doit suivre updateDoseBlockBanner(json) de PRÈS (même bloc) — "
    "écart mesuré %ld caractères, >200 suggère un autre appel, pas celui attendu",
    (long)gap);
  TEST_ASSERT_TRUE_MESSAGE(gap > 0 && gap < 200, msg);
}

// =============================================================================
// FACE B — carries (plan) : le SILENCE, cas par cas (D1)
// =============================================================================

static void test_tone_none_voie_non_couverte(void) {
  ensureLoaded();
  RowIn ph = {1, 0, "none"};
  RowIn orp = {1, 5, "fault"};
  Carries c = bannerPlan(ph, orp);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, c.ph,
    "tone === 'none' : la voie n'existe pas pour le bandeau, carries doit rester FAUX "
    "(fail-open : en cas de doute, le badge PARLE)");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.orp, "l'autre voie (fault) doit rester portée normalement");
}

static void test_rien_bloque_box_hidden_les_deux_voies_non_couvertes(void) {
  ensureLoaded();
  RowIn ph = {1, 0, "none"};
  RowIn orp = {1, 0, "none"};
  Carries c = bannerPlan(ph, orp);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, c.ph, "box.hidden global : ph non couvert");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, c.orp, "box.hidden global : orp non couvert");
}

static void test_fusion_cause_racine_ph_vers_orp_couvre_les_deux_voies(void) {
  ensureLoaded();
  // Les TROIS codes de F3 côté pH (feature-152 : réduits de 6 à 3), combinés
  // au code-garde 250 côté ORP.
  for (int i = 0; i < g_f3Count; ++i) {
    RowIn ph = {1, g_f3[i], "normal"};
    RowIn orp = {1, 250, "normal"};  // toneOrp du code 250 = 'normal'
    Carries c = bannerPlan(ph, orp);
    char msg[160];
    snprintf(msg, sizeof(msg),
      "fusion cause-racine (ph.code=%d, orp.code=250) doit couvrir LES DEUX voies", g_f3[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.ph, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.orp, msg);
  }
}

static void test_fusion_meme_code_couvre_les_deux_voies(void) {
  ensureLoaded();
  RowIn ph = {1, 9, "normal"};
  RowIn orp = {1, 9, "normal"};
  Carries c = bannerPlan(ph, orp);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.ph, "fusion même-code : ph couvert");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.orp, "fusion même-code : orp couvert");
}

static void test_fusion_badge_muet_sur_les_deux_voies(void) {
  ensureLoaded();
  // Scénario de production (feature-148 §contexte) : ph bloqué en attente de
  // mesure (code 15), orp en garde (250) — fusionnés en UNE ligne.
  RowIn ph = {1, 15, "normal"};
  RowIn orp = {1, 250, "normal"};
  Carries c = bannerPlan(ph, orp);
  // Côté ORP, 250 n'est PAS dans F1 : seule `carries.orp` ferme le badge.
  TEST_ASSERT_FALSE_MESSAGE(
    badgePushesBlockLabel("Sécurité pH/ORP active", 250, c.orp),
    "après fusion, le badge ORP doit rester muet sur la cause 250 (portée par le bandeau)");
  // Côté pH, 15 est aussi dans F1 — les deux mécanismes convergent, mais le
  // résultat observable attendu est le même : silence.
  TEST_ASSERT_FALSE_MESSAGE(
    badgePushesBlockLabel("Les mesures se mettent en place — environ une minute", 15, c.ph),
    "après fusion, le badge pH doit rester muet sur la cause 15");
}

static void test_code_inconnu_porte_par_bandeau_donc_badge_muet(void) {
  ensureLoaded();
  RowIn ph = {1, 999, "unknown"};  // « Cause inconnue (code 999) »
  RowIn orp = {0, 0, "none"};
  Carries c = bannerPlan(ph, orp);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.ph, "un code inconnu, tone!=='none', doit être porté par le bandeau");
  TEST_ASSERT_FALSE_MESSAGE(
    badgePushesBlockLabel("Cause inconnue (code 999)", 999, c.ph),
    "le doute doit rester visible EN HAUT (bandeau) seulement — le badge se tait");
}

static void test_code_undefined_porte_par_bandeau_donc_badge_muet(void) {
  ensureLoaded();
  // code === undefined → _doseBlockInfo renvoie tone='unknown' si un texte
  // serveur existe. Sentinelle -1 : aucun code réel ne la percute.
  RowIn ph = {1, -1, "unknown"};
  RowIn orp = {0, 0, "none"};
  Carries c = bannerPlan(ph, orp);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.ph, "code===undefined avec texte serveur : porté par le bandeau");
  TEST_ASSERT_FALSE_MESSAGE(
    badgePushesBlockLabel("texte brut du firmware", -1, c.ph),
    "code===undefined : le badge se tait, le bandeau porte le doute");
}

// =============================================================================
// FACE C — badge (F1 + guard) : silence / parole, scénarios discriminants
// =============================================================================

static void test_deja_source_dans_F1_reste_muet_meme_non_couvert(void) {
  ensureLoaded();
  // code 15 est dans F1 (« déjà sourcé ailleurs ») : muet MÊME SI le bandeau
  // ne le porte pas (carries=false) — question statique, indépendante de la
  // question dynamique du bandeau.
  TEST_ASSERT_FALSE_MESSAGE(
    badgePushesBlockLabel("Les mesures se mettent en place — environ une minute", 15, /*carries=*/0),
    "un code DANS F1 reste muet même si le bandeau ne le porte pas");
}

static void test_non_couvert_le_badge_parle(void) {
  ensureLoaded();
  // code 9 (régulation non automatique) : ni dans F1, ni couvert par le
  // bandeau ⇒ le badge DOIT parler.
  TEST_ASSERT_TRUE_MESSAGE(
    badgePushesBlockLabel("Régulation non automatique", 9, /*carries=*/0),
    "non sourcé ailleurs + non couvert par le bandeau : le badge doit parler");
}

static void test_couvert_par_le_bandeau_le_badge_se_tait(void) {
  ensureLoaded();
  // code 1 (watchdog) : pas dans F1, mais EFFECTIVEMENT porté par le bandeau
  // ⇒ silence (c'est tout l'objet de feature-148, cas signalé initialement
  // sur le code 250, généralisé ici à un autre code hors Set).
  TEST_ASSERT_FALSE_MESSAGE(
    badgePushesBlockLabel("Watchdog inactif — dosage suspendu", 1, /*carries=*/1),
    "porté par le bandeau : le badge doit se taire, même hors F1");
}

// test_silence_sans_porteur_badge_doit_parler_cas_filter_alarm RETIRÉ
// (feature-152, seconde passe) : le scénario FilterAlarm n'existe plus
// (phFilterAlarm/orpFilterAlarm ont disparu de data/app.js avec le mécanisme
// de filtre qu'ils signalaient). L'invariant qu'il protégeait (« silence SANS
// porteur interdit ») reste couvert par les autres scénarios de la FACE C.

// =============================================================================
// FACE D — quotas (10/11/12) : exception, toujours dupliqués
// =============================================================================

// B2 : les quotas sont poussés par un chemin SÉPARÉ (le forEach de
// dosingLimitSegments, lignes ~3033-3035), jamais gaté par
// doseBlockBannerCarries(). On le vérifie littéralement : le bloc existe en
// UN seul exemplaire, et ne mentionne PAS la garde dans sa portée immédiate.
static void test_quotas_bloc_unique_et_independant_de_carries(void) {
  ensureLoaded();
  const char* anchor = "[[dailyLimit, 'Limite journali\xc3\xa8re atteinte']";
  const int occurrences = countIn(g_app, anchor);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, occurrences,
    "le bloc quotas (dailyLimit/hourlyLimit/cyclesLimit) doit exister en UN SEUL exemplaire");

  const char* a = strstr(g_app, anchor);
  TEST_ASSERT_NOT_NULL(a);
  if (a == NULL) return;
  const char* end = strstr(a, "forEach");
  TEST_ASSERT_NOT_NULL_MESSAGE(end, "le bloc quotas doit se terminer par un .forEach(...)");
  if (end == NULL) return;
  const char* closing = strstr(end, ");");
  const size_t span = (closing != NULL) ? (size_t)(closing - a) : (size_t)(end - a + 40);
  char* region = (char*)malloc(span + 1);
  memcpy(region, a, span);
  region[span] = '\0';
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(region, "doseBlockBannerCarries"),
    "le chemin quotas ne doit JAMAIS consulter doseBlockBannerCarries — c'est l'exception "
    "volontaire de la spec feature-148 (B2), toujours dupliquée");
  free(region);
}

static void test_quota_code_11_reste_muet_sur_le_chemin_generique_meme_non_couvert(void) {
  ensureLoaded();
  // Le libellé DÉDIÉ du quota (poussé par le forEach, hors du chemin générique
  // que ce mirroir teste) existe TOUJOURS. Ce test porte sur le second
  // libellé, générique (block.label, chemin gaté par F1+carries) : il ne doit
  // PAS s'ajouter, même quand le bandeau ne couvre pas cette voie (carries=0)
  // — sans quoi un seul fait produirait DEUX segments.
  TEST_ASSERT_FALSE_MESSAGE(
    badgePushesBlockLabel("Limite horaire atteinte", 11, /*carries=*/0),
    "code 11 (quota horaire) : le chemin GÉNÉRIQUE doit rester muet (F1 l'exclut déjà) — "
    "sinon le libellé générique s'ajoute au libellé dédié, deux segments pour un seul fait");
}

// =============================================================================
// FACE E — invariant structurel B3 : buildSensorSegments() n'a que 2 appelants,
// tous deux dans updateSensorBadges() (le pivot qui rend /ph et /orp inchangés)
// =============================================================================

static void test_b3_buildSensorSegments_exactement_deux_appels_dans_updateSensorBadges(void) {
  ensureLoaded();
  const char* callAnchor = "buildSensorSegments(";
  const char* defAnchor = "function buildSensorSegments(";

  // Compte les occurrences de l'APPEL (exclut la définition elle-même).
  int callCount = 0;
  const size_t an = strlen(callAnchor);
  const size_t fullLen = strlen(g_app);
  for (size_t i = 0; i + an <= fullLen; ++i) {
    if (memcmp(g_app + i, callAnchor, an) != 0) continue;
    const int isDef = (i >= strlen(defAnchor) - an) &&
      (memcmp(g_app + i - (strlen(defAnchor) - an), defAnchor, strlen(defAnchor)) == 0);
    if (!isDef) callCount++;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, callCount,
    "buildSensorSegments() doit avoir EXACTEMENT 2 appelants — tout appel supplémentaire "
    "(ex. depuis /ph ou /orp) romprait l'invariant qui garantit ces pages inchangées "
    "(feature-148 §correction de l'audit)");

  const char* bodyStart = strstr(g_app, "function updateSensorBadges(");
  TEST_ASSERT_NOT_NULL(bodyStart);
  const char* bodyEnd = strstr(g_app, "function fmtInjectedMl(");
  TEST_ASSERT_NOT_NULL(bodyEnd);
  if (bodyStart == NULL || bodyEnd == NULL) return;

  // Les deux appels (et seulement eux) doivent tomber dans cette plage.
  int callsInRange = 0;
  for (const char* q = bodyStart; q + an <= bodyEnd; ++q) {
    if (memcmp(q, callAnchor, an) == 0) callsInRange++;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, callsInRange,
    "les 2 appels à buildSensorSegments() doivent être DANS updateSensorBadges()");
}

// =============================================================================
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_f1_contenu_attendu);
  RUN_TEST(test_f3_contenu_attendu);
  RUN_TEST(test_f2_garde_est_negatee);
  RUN_TEST(test_f4_contenu_attendu);
  // test_f6_garde_filter_alarm_presente retiré (feature-152 : garde + FilterAlarm disparus).
  RUN_TEST(test_f7_bandeau_mis_a_jour_avant_les_badges);

  RUN_TEST(test_tone_none_voie_non_couverte);
  // test_filter_alarm_voie_non_couverte_quel_que_soit_le_code retiré (idem).
  RUN_TEST(test_rien_bloque_box_hidden_les_deux_voies_non_couvertes);
  RUN_TEST(test_fusion_cause_racine_ph_vers_orp_couvre_les_deux_voies);
  RUN_TEST(test_fusion_meme_code_couvre_les_deux_voies);
  RUN_TEST(test_fusion_badge_muet_sur_les_deux_voies);
  RUN_TEST(test_code_inconnu_porte_par_bandeau_donc_badge_muet);
  RUN_TEST(test_code_undefined_porte_par_bandeau_donc_badge_muet);

  RUN_TEST(test_deja_source_dans_F1_reste_muet_meme_non_couvert);
  RUN_TEST(test_non_couvert_le_badge_parle);
  RUN_TEST(test_couvert_par_le_bandeau_le_badge_se_tait);
  // test_silence_sans_porteur_badge_doit_parler_cas_filter_alarm retiré (idem).

  RUN_TEST(test_quotas_bloc_unique_et_independant_de_carries);
  RUN_TEST(test_quota_code_11_reste_muet_sur_le_chemin_generique_meme_non_couvert);

  RUN_TEST(test_b3_buildSensorSegments_exactement_deux_appels_dans_updateSensorBadges);
  return UNITY_END();
}
