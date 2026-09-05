// =============================================================================
// SENTINELLE DE PARITÉ — /get-config  ↔  trame WebSocket `config`  (bug-089)
// =============================================================================
// Ce n'est pas un test de comportement : c'est un test STRUCTUREL, qui lit les
// SOURCES du projet. Il existe parce que le motif « un champ ajouté d'un seul
// côté » en est à sa 4e occurrence documentée dans ce dépôt. Corriger l'écart
// sans garde structurelle garantissait une 5e.
//
// ANGLE MORT CONNU (relevé en revue) : l'extraction est TEXTUELLE. Une clé
// mise en COMMENTAIRE entre les marqueurs reste comptée comme présente. La
// sentinelle protège donc contre l'oubli, pas contre la désactivation
// délibérée — ce qui est le bon périmètre : elle vise la distraction, pas la
// malveillance. Ne pas essayer de parser du C++ ici, le remède serait pire.
//
// CE QU'IL VÉRIFIE
//   (a) toute clé servie par /get-config est SOIT dans la trame WS, SOIT
//       déclarée HTTP-seule dans kHttpOnlyKeys AVEC un motif écrit ;
//   (b) aucune clé n'est WS-seule — c'est le sens DANGEREUX du motif : un champ
//       présent en WS et absent de /get-config ne « manque » pas, il CLIGNOTE
//       (la charge /get-config remplace périodiquement l'objet de config côté
//       client et efface le champ, puis la trame WS le remet) ;
//   (c) les marqueurs d'ancrage existent des DEUX côtés — leur disparition rend
//       le test aveugle, donc elle est un ÉCHEC EXPLICITE ;
//   (d) aucune exception MORTE : une clé de kHttpOnlyKeys qui n'existe plus dans
//       /get-config fait échouer le test. C'est ce qui empêche la liste de
//       pourrir en décharge de justifications périmées.
//
// CRITÈRE DE PARTAGE (ce qui décide si une clé va dans la trame ou non) :
//   la trame porte l'état MUTABLE À L'EXÉCUTION et NON DÉRIVABLE.
//   Tout le reste est HTTP-seul et DÉCLARÉ. La parité intégrale serait pire que
//   le bug : +1365 o sur une trame plafonnée DUR à kWsFrameBufBytes = 3584 o,
//   et un dépassement fait SAUTER le push (la garde ne tronque jamais).
//
// CONVENTION NORMATIVE LUE PAR CE TEST
//   src/web_routes_config.cpp : entre les marqueurs, receveur `doc[` ;
//   src/ws_manager.cpp        : entre les marqueurs, receveur `d[`.
//   Marqueurs : `>>> PARITE-CONFIG:DEBUT` et `<<< PARITE-CONFIG:FIN`.
//
// LE PIRE CAS DE CETTE SENTINELLE, C'EST SA DÉSACTIVATION. Chaque message
// d'échec dit donc QUOI FAIRE, et jamais « ignorer ce test ».
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Table des clés VOLONTAIREMENT HTTP-seules. Motif OBLIGATOIRE (≥ 20 caractères,
// asserté) : une exception sans justification lisible est une exception qu'on
// ne saura pas réévaluer dans six mois.
// ---------------------------------------------------------------------------
struct HttpOnlyKey {
  const char* key;
  const char* reason;
};

static const HttpOnlyKey kHttpOnlyKeys[] = {
  // Constantes de compilation (constants.h) : elles ne changent JAMAIS à
  // l'exécution, les pousser en boucle dépenserait le budget de trame pour rien.
  // Justification d'origine : src/web_routes_config.cpp, bloc « BORNES DE
  // RÉGLAGE pH servies au client » (v2.25.0, B1).
  {"ph_target_min",  "Constante de compilation (kPhTargetMin) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"ph_target_max",  "Constante de compilation (kPhTargetMax) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"ph_target_step", "Constante de compilation (kPhTargetStep) : immuable a l'execution, app.js conserve le dernier pas recu."},
  {"ph_offset_min",  "Constante de compilation (kPhOffsetMin) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"ph_offset_max",  "Constante de compilation (kPhOffsetMax) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"ph_offset_step", "Constante de compilation (kPhOffsetStep) : immuable a l'execution, app.js conserve le dernier pas recu."},
  {"regulation_defaults_by_size",
   "Table de compilation (kRegulationDefaultsBySize, 3 colonnes) : immuable a l'execution et ~525 o de trame par push."},
  // Dérivable côté client : `pool_size` EST dans la trame WS depuis bug-089 et
  // `regulation_defaults_by_size` est mis en cache par app.js — le client
  // reconstruit la colonne courante (syncRegulationDefaults). L'extraire du
  // fichier de routes coûterait +703 o de trame pour une valeur calculable.
  {"regulation_defaults",
   "Derivable cote client : pool_size est dans la trame WS et by_size est en cache (app.js syncRegulationDefaults)."},
};
static const size_t kHttpOnlyCount = sizeof(kHttpOnlyKeys) / sizeof(kHttpOnlyKeys[0]);

static const size_t kMinReasonLen = 20;

static const char* kMarkerBegin = ">>> PARITE-CONFIG:DEBUT";
static const char* kMarkerEnd   = "<<< PARITE-CONFIG:FIN";

static const char* kHttpFile = "src/web_routes_config.cpp";
static const char* kWsFile   = "src/ws_manager.cpp";

// ---------------------------------------------------------------------------
// Lecture d'un fichier du dépôt — helper repris de
// test/test_native_time_to_target/test_time_to_target.cpp : racine reconstruite
// depuis __FILE__ (chemin tel que vu par le compilateur), avec repli sur des
// préfixes relatifs selon le répertoire courant du lanceur de tests.
// ---------------------------------------------------------------------------
static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  const char* candidates[5];
  char rooted[512];

  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_ws_config_parity");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  candidates[0] = rooted[0] ? rooted : NULL;
  candidates[1] = rel;
  candidates[2] = NULL;
  candidates[3] = NULL;
  candidates[4] = NULL;

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

// ---------------------------------------------------------------------------
// Extraction des clés `<recv>["<clé>"]` dans la région délimitée par les
// marqueurs. Retourne le nombre de clés, ou -1 si un marqueur manque (l'appelant
// transforme cela en échec explicite : test aveugle == test en échec).
// ---------------------------------------------------------------------------
#define kMaxKeys 256
#define kMaxKeyLen 64

struct KeySet {
  char keys[kMaxKeys][kMaxKeyLen];
  size_t count;
};

static bool keySetHas(const KeySet& s, const char* k) {
  for (size_t i = 0; i < s.count; ++i) {
    if (strcmp(s.keys[i], k) == 0) return true;
  }
  return false;
}

static void keySetAdd(KeySet& s, const char* k) {
  if (keySetHas(s, k) || s.count >= kMaxKeys) return;
  snprintf(s.keys[s.count], kMaxKeyLen, "%s", k);
  s.count++;
}

// -1 marqueur DEBUT absent, -2 marqueur FIN absent, -3 fichier introuvable,
// -4 marqueurs inversés. >= 0 : nombre de clés extraites.
static int extractKeys(const char* rel, const char* recv, KeySet& out) {
  out.count = 0;
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src == NULL) return -3;

  const char* b = strstr(src, kMarkerBegin);
  const char* e = strstr(src, kMarkerEnd);
  if (b == NULL) { free(src); return -1; }
  if (e == NULL) { free(src); return -2; }
  if (e < b)     { free(src); return -4; }

  // Motif recherché : <recv>["  (le receveur est imposé par la convention
  // normative écrite dans les deux fichiers source).
  char needle[32];
  snprintf(needle, sizeof(needle), "%s[\"", recv);
  const size_t nl = strlen(needle);

  for (const char* p = b; p + nl < e; ++p) {
    // Le receveur doit être un identifiant complet : le caractère qui précède
    // ne doit pas être une lettre/chiffre/underscore (sinon `bySize["small"]`
    // serait capté par le receveur `d`).
    if (p > src) {
      const char c = p[-1];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_') continue;
    }
    if (strncmp(p, needle, nl) != 0) continue;
    const char* k = p + nl;
    char buf[kMaxKeyLen];
    size_t i = 0;
    while (k[i] && k[i] != '"' && i < kMaxKeyLen - 1) {
      const char c = k[i];
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_';
      if (!ok) break;
      buf[i] = c;
      ++i;
    }
    if (i == 0 || k[i] != '"' || k[i + 1] != ']') continue;
    buf[i] = '\0';
    keySetAdd(out, buf);
  }
  free(src);
  return (int)out.count;
}

// Message d'échec commun aux problèmes de marqueur : il dit explicitement de
// RESTAURER le marqueur, jamais de désactiver le test.
static void failOnExtractError(const char* rel, const char* recv, int rc) {
  char msg[512];
  const char* what =
    rc == -1 ? "marqueur de DEBUT « >>> PARITE-CONFIG:DEBUT » ABSENT" :
    rc == -2 ? "marqueur de FIN « <<< PARITE-CONFIG:FIN » ABSENT" :
    rc == -3 ? "fichier source INTROUVABLE" :
    rc == -4 ? "marqueurs INVERSES (FIN avant DEBUT)" : "erreur inconnue";
  snprintf(msg, sizeof(msg),
           "SENTINELLE AVEUGLE dans %s : %s. Sans les marqueurs, aucune clé "
           "n'est comparée et la parité /get-config <-> trame WS `config` cesse "
           "d'être garantie (5e occurrence du motif). QUE FAIRE : RESTAURER le "
           "marqueur autour du bloc de clés (receveur `%s[`) — NE PAS "
           "DESACTIVER CE TEST.",
           rel, what, recv);
  TEST_FAIL_MESSAGE(msg);
}

// ---------------------------------------------------------------------------
// (c) Marqueurs présents des DEUX côtés, et clés effectivement extraites.
// ---------------------------------------------------------------------------
void test_c_marqueurs_presents_des_deux_cotes(void) {
  KeySet http, ws;
  const int rcHttp = extractKeys(kHttpFile, "doc", http);
  if (rcHttp < 0) { failOnExtractError(kHttpFile, "doc", rcHttp); return; }
  const int rcWs = extractKeys(kWsFile, "d", ws);
  if (rcWs < 0) { failOnExtractError(kWsFile, "d", rcWs); return; }

  // Contrôle NÉGATIF : un extracteur qui ne trouve rien « passerait » toutes
  // les autres assertions. Un plancher franc rend ce silence impossible.
  char msg[384];
  snprintf(msg, sizeof(msg),
           "Extraction SUSPECTE : seulement %d clé(s) `doc[\"...\"]` entre les "
           "marqueurs de %s. Soit la convention normative (receveur `doc[`) a "
           "été rompue, soit les marqueurs encadrent le mauvais bloc. QUE "
           "FAIRE : replacer les marqueurs autour du bloc de clés — NE PAS "
           "DESACTIVER CE TEST.", rcHttp, kHttpFile);
  TEST_ASSERT_TRUE_MESSAGE(rcHttp >= 60, msg);

  snprintf(msg, sizeof(msg),
           "Extraction SUSPECTE : seulement %d clé(s) `d[\"...\"]` entre les "
           "marqueurs de %s. Soit la convention normative (receveur `d[`) a été "
           "rompue, soit les marqueurs encadrent le mauvais bloc. QUE FAIRE : "
           "replacer les marqueurs autour du bloc de clés — NE PAS DESACTIVER "
           "CE TEST.", rcWs, kWsFile);
  TEST_ASSERT_TRUE_MESSAGE(rcWs >= 60, msg);
}

// ---------------------------------------------------------------------------
// (a) Aucune clé HTTP non couverte : elle est dans la trame WS, ou déclarée.
// ---------------------------------------------------------------------------
void test_a_aucune_cle_http_non_couverte(void) {
  KeySet http, ws;
  const int rcHttp = extractKeys(kHttpFile, "doc", http);
  if (rcHttp < 0) { failOnExtractError(kHttpFile, "doc", rcHttp); return; }
  const int rcWs = extractKeys(kWsFile, "d", ws);
  if (rcWs < 0) { failOnExtractError(kWsFile, "d", rcWs); return; }

  for (size_t i = 0; i < http.count; ++i) {
    const char* k = http.keys[i];
    if (keySetHas(ws, k)) continue;
    bool declared = false;
    for (size_t j = 0; j < kHttpOnlyCount; ++j) {
      if (strcmp(kHttpOnlyKeys[j].key, k) == 0) { declared = true; break; }
    }
    char msg[768];
    snprintf(msg, sizeof(msg),
             "PARITE ROMPUE : la clé « %s » est servie par /get-config "
             "(%s) mais ABSENTE de la trame WebSocket `config` (%s) et NON "
             "DECLAREE. Les autres clients WS ne verront jamais ce champ "
             "changer. QUE FAIRE — au choix : (1) AJOUTER la clé à "
             "_buildConfigJson, ENTRE LES MARQUEURS, si son état est mutable à "
             "l'exécution et non dérivable (vérifier le budget : marge sous "
             "kWsFrameBufBytes = 3584 o, un dépassement SAUTE le push) ; OU "
             "(2) L'INSCRIRE dans kHttpOnlyKeys de ce fichier AVEC un motif "
             "écrit d'au moins %u caractères. NE PAS DESACTIVER CE TEST.",
             k, kHttpFile, kWsFile, (unsigned)kMinReasonLen);
    TEST_ASSERT_TRUE_MESSAGE(declared, msg);
  }
}

// ---------------------------------------------------------------------------
// (b) Aucune clé WS-seule — le sens DANGEREUX du motif : ça clignote.
// ---------------------------------------------------------------------------
void test_b_aucune_cle_ws_seule(void) {
  KeySet http, ws;
  const int rcHttp = extractKeys(kHttpFile, "doc", http);
  if (rcHttp < 0) { failOnExtractError(kHttpFile, "doc", rcHttp); return; }
  const int rcWs = extractKeys(kWsFile, "d", ws);
  if (rcWs < 0) { failOnExtractError(kWsFile, "d", rcWs); return; }

  for (size_t i = 0; i < ws.count; ++i) {
    const char* k = ws.keys[i];
    char msg[768];
    snprintf(msg, sizeof(msg),
             "PARITE ROMPUE (SENS DANGEREUX) : la clé « %s » est poussée par la "
             "trame WebSocket `config` (%s) mais N'EST PAS servie par "
             "/get-config (%s). Un champ WS-seul ne disparaît pas : il "
             "CLIGNOTE — la charge /get-config remplace périodiquement l'objet "
             "de config côté client et l'efface, la trame WS le remet. QUE "
             "FAIRE : AJOUTER la clé à handleGetConfig, ENTRE LES MARQUEURS "
             "(receveur `doc[`), avec la MEME orthographe. Une clé HTTP-seule "
             "peut être déclarée dans kHttpOnlyKeys ; une clé WS-seule, JAMAIS. "
             "NE PAS DESACTIVER CE TEST.",
             k, kWsFile, kHttpFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(http, k), msg);
  }
}

// ---------------------------------------------------------------------------
// (d) Aucune exception MORTE : la liste ne doit pas pourrir.
// ---------------------------------------------------------------------------
void test_d_aucune_exception_morte(void) {
  KeySet http, ws;
  const int rcHttp = extractKeys(kHttpFile, "doc", http);
  if (rcHttp < 0) { failOnExtractError(kHttpFile, "doc", rcHttp); return; }
  const int rcWs = extractKeys(kWsFile, "d", ws);
  if (rcWs < 0) { failOnExtractError(kWsFile, "d", rcWs); return; }

  for (size_t j = 0; j < kHttpOnlyCount; ++j) {
    const char* k = kHttpOnlyKeys[j].key;
    char msg[512];

    snprintf(msg, sizeof(msg),
             "EXCEPTION MORTE : « %s » est déclarée HTTP-seule dans "
             "kHttpOnlyKeys, mais cette clé n'est PLUS servie par /get-config "
             "(%s). Une exception qui ne protège plus rien transforme la liste "
             "en décharge de justifications périmées, et masquera la prochaine "
             "vraie divergence. QUE FAIRE : SUPPRIMER l'entrée de "
             "kHttpOnlyKeys, ou restaurer la clé côté HTTP si son retrait est "
             "un accident. NE PAS DESACTIVER CE TEST.",
             k, kHttpFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(http, k), msg);

    // Corollaire : une exception ne doit pas non plus dire l'inverse de la
    // réalité — si la clé a fini par entrer dans la trame WS, l'exception ment.
    snprintf(msg, sizeof(msg),
             "EXCEPTION CONTREDITE : « %s » est déclarée HTTP-seule dans "
             "kHttpOnlyKeys alors qu'elle EST désormais poussée par la trame WS "
             "(%s). Le motif écrit ne décrit plus le code. QUE FAIRE : "
             "SUPPRIMER l'entrée de kHttpOnlyKeys (la parité est atteinte pour "
             "cette clé). NE PAS DESACTIVER CE TEST.",
             k, kWsFile);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(ws, k), msg);
  }
}

// ---------------------------------------------------------------------------
// Motif obligatoire : une exception sans justification lisible n'en est pas une.
// ---------------------------------------------------------------------------
void test_e_chaque_exception_porte_un_motif_ecrit(void) {
  for (size_t j = 0; j < kHttpOnlyCount; ++j) {
    const char* k = kHttpOnlyKeys[j].key;
    const char* r = kHttpOnlyKeys[j].reason;
    char msg[512];

    snprintf(msg, sizeof(msg),
             "EXCEPTION SANS MOTIF : « %s » n'a pas de justification écrite. "
             "QUE FAIRE : écrire POURQUOI cette clé n'a pas sa place dans une "
             "trame poussée en boucle (constante de compilation ? valeur "
             "dérivable côté client ?) — au moins %u caractères. NE PAS "
             "DESACTIVER CE TEST.", k, (unsigned)kMinReasonLen);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, msg);
    TEST_ASSERT_TRUE_MESSAGE(strlen(r) >= kMinReasonLen, msg);

    // Doublon : deux motifs pour une même clé = un des deux sera ignoré.
    for (size_t i = 0; i < j; ++i) {
      snprintf(msg, sizeof(msg),
               "EXCEPTION EN DOUBLE : « %s » apparaît deux fois dans "
               "kHttpOnlyKeys. QUE FAIRE : fusionner les deux entrées en une "
               "seule, avec le motif le plus complet.", k);
      TEST_ASSERT_FALSE_MESSAGE(strcmp(kHttpOnlyKeys[i].key, k) == 0, msg);
    }
  }
}

// ---------------------------------------------------------------------------
// Ancrage chiffré de l'état connu au moment de bug-089 (v2.39.0) : 89 clés HTTP,
// 81 clés WS, 8 exceptions. Ce test ne cherche pas l'égalité éternelle — il
// exige juste que le décompte des exceptions reste EXACTEMENT celui de la table.
// ---------------------------------------------------------------------------
void test_f_inventaire_coherent(void) {
  KeySet http, ws;
  const int rcHttp = extractKeys(kHttpFile, "doc", http);
  if (rcHttp < 0) { failOnExtractError(kHttpFile, "doc", rcHttp); return; }
  const int rcWs = extractKeys(kWsFile, "d", ws);
  if (rcWs < 0) { failOnExtractError(kWsFile, "d", rcWs); return; }

  size_t httpOnly = 0;
  for (size_t i = 0; i < http.count; ++i) {
    if (!keySetHas(ws, http.keys[i])) httpOnly++;
  }
  char msg[512];
  snprintf(msg, sizeof(msg),
           "INVENTAIRE INCOHERENT : %u clé(s) HTTP-seule(s) constatée(s) dans "
           "le code pour %u entrée(s) dans kHttpOnlyKeys. QUE FAIRE : ajouter "
           "la clé manquante à la trame WS (entre les marqueurs) OU l'inscrire "
           "dans kHttpOnlyKeys avec un motif ; si une exception est devenue "
           "inutile, la supprimer. NE PAS DESACTIVER CE TEST.",
           (unsigned)httpOnly, (unsigned)kHttpOnlyCount);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(kHttpOnlyCount, httpOnly, msg);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_c_marqueurs_presents_des_deux_cotes);
  RUN_TEST(test_a_aucune_cle_http_non_couverte);
  RUN_TEST(test_b_aucune_cle_ws_seule);
  RUN_TEST(test_d_aucune_exception_morte);
  RUN_TEST(test_e_chaque_exception_porte_un_motif_ecrit);
  RUN_TEST(test_f_inventaire_coherent);
  return UNITY_END();
}
