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
  {"ph_offset_step", "Constante de compilation (kPhOffsetStep) : immuable a l'execution, app.js conserve le dernier pas recu."},
  // feature-168 : ph_offset_min/max SUPPRIMES (plus de borne de reglage,
  // decision utilisateur du 2026-10-16). Les 5 constantes qui remplacent
  // l'ancienne paire sont, comme elle, des constantes de compilation --
  // memes regles, ADR-0100 (seuils cites par les textes d'avertissement de
  // l'ecran de reglage, jamais ecrits en dur cote client).
  {"ph_offset_unusual_abs",            "Constante de compilation (kPhOffsetUnusualAbs) : immuable a l'execution, app.js conserve le dernier seuil recu."},
  {"ph_offset_confirm_interval_days",  "Constante de compilation (kPhOffsetConfirmIntervalDays) : immuable a l'execution, app.js conserve le dernier seuil recu."},
  {"ph_offset_impasse_threshold",      "Constante de compilation (kPhOffsetImpasseThreshold) : immuable a l'execution, app.js conserve le dernier seuil recu."},
  {"ph_offset_impasse_threshold_high", "Constante de compilation (kPhOffsetImpasseThresholdHigh) : immuable a l'execution, app.js conserve le dernier seuil recu."},
  {"ph_raw_floor_absolute",            "Constante de compilation (kPhRawFloorAbsolute) : immuable a l'execution, app.js conserve le dernier seuil recu."},
  {"ph_raw_ceiling_absolute",          "Constante de compilation (kPhRawCeilingAbsolute) : immuable a l'execution, app.js conserve le dernier seuil recu."},
  // feature-164 (gate pool-chemistry, 3e passage, ADR-0100) : bornes de la
  // consigne ORP servies au client, miroir EXACT du triplet ph_target_*
  // ci-dessus -- memes regles (constante de compilation, jamais en WS).
  {"orp_target_min",  "Constante de compilation (kOrpTargetPlausibleMin) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"orp_target_max",  "Constante de compilation (kOrpTargetPlausibleMax) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"orp_target_step", "Constante de compilation (kOrpTargetStep) : immuable a l'execution, app.js conserve le dernier pas recu."},
  // feature-154 : fenetre de sante EZO (constants.h) servie pour les 3
  // pastilles de l'ecran "Etat sonde pH" (classifyPhCriterion cote client).
  // Memes regles que les bornes ph_target_*/ph_offset_* ci-dessus.
  {"ph_slope_healthy_min_pct", "Constante de compilation (kPhSlopeHealthyMinPct) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"ph_slope_healthy_max_pct", "Constante de compilation (kPhSlopeHealthyMaxPct) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"ph_zero_healthy_max_mv",   "Constante de compilation (kPhZeroHealthyMaxMv) : immuable a l'execution, app.js conserve la derniere borne recue."},
  // feature-155 : constantes de l'accord pH sur un test manuel, consommees par
  // la fonction PURE du navigateur (apercu seulement, cf. condition 1 du gate
  // pool-chemistry -- le calcul qui compte est serveur, POST /tune-ph-offset).
  // Memes regles que les bornes ph_target_*/ph_offset_* ci-dessus.
  {"ph_cal_points_min",         "Constante de compilation (kPhTuneCalPointsMin) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"ph_tune_min_filtration_ms", "Constante de compilation (kPhTuneMinFiltrationMs) : immuable a l'execution, app.js conserve la derniere borne recue."},
  // feature-164 (gate pool-chemistry C12, ADR-0100) : seuil d'avertissement
  // de duree de filtration courte, meme regle que les bornes ph_target_*
  // ci-dessus -- constante de compilation, jamais en WS.
  {"filtration_short_duration_warn_min",
   "Constante de compilation (kFiltrationShortDurationWarnMinutes) : immuable a l'execution, app.js conserve la derniere borne recue."},
  {"regulation_defaults_by_size",
   "Table de compilation (kRegulationDefaultsBySize, 3 colonnes) : immuable a l'execution et ~525 o de trame par push."},
  // Dérivable côté client : `pool_size` EST dans la trame WS depuis bug-089 et
  // `regulation_defaults_by_size` est mis en cache par app.js — le client
  // reconstruit la colonne courante (syncRegulationDefaults). L'extraire du
  // fichier de routes coûterait +703 o de trame pour une valeur calculable.
  {"regulation_defaults",
   "Derivable cote client : pool_size est dans la trame WS et by_size est en cache (app.js syncRegulationDefaults)."},
  // feature-110 : compteurs d'HYGIENE de la comptabilite (ticks anormalement
  // longs et volume credité par eux). Ils qualifient un releve de bidon, ils ne
  // pilotent aucun affichage continu : ils sont lus UNE fois par campagne, au
  // changement de bidon. Les pousser toutes les 5 s couterait ~120 o de trame
  // (plafond DUR kWsFrameBufBytes = 3584 o) pour zero usage temps reel. Les
  // compteurs de DEMARRAGES (`ph_starts`, `orp_starts`), eux, SONT dans la
  // trame : ils avancent en continu a cote du volume.
  {"ph_anom",
   "Hygiene de mesure lue une fois par campagne (releve de bidon) : aucun usage temps reel, ~30 o de trame economises."},
  {"orp_anom",
   "Hygiene de mesure lue une fois par campagne (releve de bidon) : aucun usage temps reel, ~30 o de trame economises."},
  {"ph_anom_ml",
   "Hygiene de mesure lue une fois par campagne (releve de bidon) : aucun usage temps reel, ~30 o de trame economises."},
  {"orp_anom_ml",
   "Hygiene de mesure lue une fois par campagne (releve de bidon) : aucun usage temps reel, ~30 o de trame economises."},
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
// (g) bug-161 (gate pool-chemistry C-2, BLOQUANTE) : `orp_enabled` est un
// DÉRIVÉ PUR du mode (`orpRegulationMode != "manual"`), SUR TOUTE SÉQUENCE —
// jamais une valeur acceptée telle quelle depuis une requête. Avant ce
// correctif, `mqttCfg.orpEnabled = doc["orp_enabled"];` (web_routes_config.cpp)
// acceptait la valeur BRUTE du client : un payload `{"orp_enabled": true}`
// SEUL (sans `orp_regulation_mode`) pouvait réarmer `orpEnabled` pendant que
// `orpRegulationMode` restait "manual" — exactement la divergence
// mode/enabled que ce lot ferme, miroir du correctif `ph_enabled` (v2.25.0 B7).
//
// Test STRUCTUREL (même famille que (a)-(f) ci-dessus) : il lit les SOURCES,
// pas le comportement — ce fichier ne compile pas Arduino/PubSubClient/
// ESPAsyncWebServer nativement. Il vérifie, texte normalisé (espaces
// retirés) :
//   - AUCUNE occurrence du motif dangereux `mqttCfg.orpEnabled=doc[...]`
//     (écriture directe depuis le corps de la requête) dans
//     src/web_routes_config.cpp — le mutant « réintroduire l'écriture
//     directe » (ligne supprimée par ce lot) tue ce test ;
//   - EXACTEMENT une affectation à `mqttCfg.orpEnabled=` dans
//     web_routes_config.cpp, et c'est la forme dérivée
//     `(orpRegMode!="manual")` — le mutant « ajouter une seconde affectation »
//     (second chemin d'écriture) tue ce test ;
//   - EXACTEMENT une affectation à `mqttCfg.orpEnabled=` dans
//     src/mqtt_manager.cpp (chemin MQTT), et c'est la forme dérivée
//     `(payloadStr!="manual")` — même garantie côté MQTT.
// ---------------------------------------------------------------------------
static const char* kMqttFile = "src/mqtt_manager.cpp";

// Copie espaces/tabs/retours-ligne retirés — mêmes garanties que
// extractNormalizedRhs (test_ws_data_parity.cpp) : une reformulation
// (saut de ligne, indentation) ne doit jamais faire paraître absent un motif
// présent, ni inversement masquer sa duplication.
static char* stripWhitespace(const char* src) {
  const size_t len = strlen(src);
  char* out = (char*)malloc(len + 1);
  if (out == NULL) return NULL;
  size_t n = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = src[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    out[n++] = c;
  }
  out[n] = '\0';
  return out;
}

// Nombre d'occurrences NON chevauchantes de `needle` dans `haystack`.
static size_t countOccurrences(const char* haystack, const char* needle) {
  size_t count = 0;
  const char* p = haystack;
  const size_t nl = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) {
    count++;
    p += nl;
  }
  return count;
}

void test_g_orp_enabled_derive_jamais_ecrit_directement(void) {
  long lenHttp = 0, lenMqtt = 0;
  char* rawHttp = readProjectFile(kHttpFile, &lenHttp);
  TEST_ASSERT_NOT_NULL_MESSAGE(rawHttp,
      "FICHIER INTROUVABLE : src/web_routes_config.cpp. QUE FAIRE : vérifier "
      "le chemin kHttpFile. NE PAS DESACTIVER CE TEST.");
  char* rawMqtt = readProjectFile(kMqttFile, &lenMqtt);
  TEST_ASSERT_NOT_NULL_MESSAGE(rawMqtt,
      "FICHIER INTROUVABLE : src/mqtt_manager.cpp. QUE FAIRE : vérifier le "
      "chemin kMqttFile. NE PAS DESACTIVER CE TEST.");
  if (rawHttp == NULL || rawMqtt == NULL) { free(rawHttp); free(rawMqtt); return; }

  char* http = stripWhitespace(rawHttp);
  char* mqtt = stripWhitespace(rawMqtt);
  free(rawHttp);
  free(rawMqtt);
  TEST_ASSERT_NOT_NULL(http);
  TEST_ASSERT_NOT_NULL(mqtt);
  if (http == NULL || mqtt == NULL) { free(http); free(mqtt); return; }

  // Mutant « réintroduire l'écriture directe » (C-2) : la ligne supprimée par
  // ce lot était `mqttCfg.orpEnabled = doc["orp_enabled"];`.
  TEST_ASSERT_NULL_MESSAGE(
      strstr(http, "mqttCfg.orpEnabled=doc["),
      "C-2 ROMPU : src/web_routes_config.cpp accepte À NOUVEAU orp_enabled en "
      "ÉCRITURE directe depuis la requête (mqttCfg.orpEnabled=doc[...]). "
      "orp_enabled DOIT rester un dérivé pur de orp_regulation_mode — "
      "supprimer cette ligne, miroir de ph_enabled (v2.25.0 B7). NE PAS "
      "DESACTIVER CE TEST.");

  // Exactement UNE affectation à mqttCfg.orpEnabled= dans chaque fichier, et
  // c'est la forme dérivée attendue — un second chemin d'écriture (même
  // dérivé) romprait quand même l'invariant « un seul endroit qui décide ».
  const size_t httpAssignCount = countOccurrences(http, "mqttCfg.orpEnabled=");
  char msg[400];
  snprintf(msg, sizeof(msg),
           "C-2 : src/web_routes_config.cpp affecte mqttCfg.orpEnabled= %u "
           "fois (attendu : exactement 1, la forme dérivée du mode). Un "
           "second chemin d'écriture romprait l'invariant même s'il est "
           "dérivé. NE PAS DESACTIVER CE TEST.", (unsigned)httpAssignCount);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(1, httpAssignCount, msg);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(http, "mqttCfg.orpEnabled=(orpRegMode!=\"manual\")"),
      "C-2 : la seule affectation de mqttCfg.orpEnabled dans "
      "web_routes_config.cpp n'est plus de la forme dérivée "
      "`(orpRegMode != \"manual\")`. QUE FAIRE : restaurer la dérivation, ou "
      "mettre ce test à jour SCIEMMENT si la variable locale a changé de nom. "
      "NE PAS DESACTIVER CE TEST.");

  const size_t mqttAssignCount = countOccurrences(mqtt, "mqttCfg.orpEnabled=");
  snprintf(msg, sizeof(msg),
           "C-1 : src/mqtt_manager.cpp affecte mqttCfg.orpEnabled= %u fois "
           "(attendu : exactement 1, la forme dérivée du payload MQTT). NE "
           "PAS DESACTIVER CE TEST.", (unsigned)mqttAssignCount);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(1, mqttAssignCount, msg);
  TEST_ASSERT_NOT_NULL_MESSAGE(
      strstr(mqtt, "mqttCfg.orpEnabled=(payloadStr!=\"manual\")"),
      "C-1 : la seule affectation de mqttCfg.orpEnabled dans "
      "mqtt_manager.cpp n'est plus de la forme dérivée "
      "`(payloadStr != \"manual\")`. QUE FAIRE : restaurer la dérivation, ou "
      "mettre ce test à jour SCIEMMENT si la variable locale a changé de nom. "
      "NE PAS DESACTIVER CE TEST.");

  free(http);
  free(mqtt);
}

// ---------------------------------------------------------------------------
// (h) feature-162 (D4, test-engineer) : PARITE D'EXPRESSION pour
// lighting_mode / lighting_enabled / lighting_schedule_enabled. Les tests
// (a)-(d) ci-dessus ne comparent que des JEUX DE CLÉS : ils ne voient pas une
// clé présente des deux côtés mais CALCULÉE DIFFÉREMMENT — même angle mort que
// test_q de test_native_ws_data_parity (feature-152, découvert par mutation).
// `window._config` (chargé par /get-config) et la trame WS `config` alimentent
// la MÊME variable côté client : une divergence d'expression ferait OSCILLER
// le mode affiché au rythme des deux cadences, sans qu'aucune charge isolée ne
// paraisse fausse.
// ---------------------------------------------------------------------------
static const char* kLightingExprKeys[] = {
  "lighting_mode", "lighting_enabled", "lighting_schedule_enabled",
};
static const size_t kLightingExprKeysCount =
    sizeof(kLightingExprKeys) / sizeof(kLightingExprKeys[0]);

// Membre droit de `recv["key"] = ... ;`, RESTREINT à la région [beginMarker,
// endMarker) du fichier `rel`. Espaces retirés. NULL si introuvable (marqueur
// absent, clé absente de la région, ou forme autre que `= ... ;`).
static char* extractNormalizedRhsInRegion(const char* rel, const char* recv, const char* key,
                                           const char* beginMarker, const char* endMarker) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src == NULL) return NULL;
  const char* b = strstr(src, beginMarker);
  const char* e = (b != NULL) ? strstr(b, endMarker) : NULL;
  if (b == NULL || e == NULL) { free(src); return NULL; }

  char needle[64];
  snprintf(needle, sizeof(needle), "%s[\"%s\"]", recv, key);
  const char* p = strstr(b, needle);
  if (p == NULL || p >= e) { free(src); return NULL; }
  p += strlen(needle);
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '=') { free(src); return NULL; }
  p++;
  const char* semi = strchr(p, ';');
  if (semi == NULL || semi > e) { free(src); return NULL; }

  char* out = (char*)malloc((size_t)(semi - p) + 1);
  if (out == NULL) { free(src); return NULL; }
  size_t n = 0;
  for (const char* q = p; q < semi; ++q) {
    if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') continue;
    out[n++] = *q;
  }
  out[n] = '\0';
  free(src);
  return out;
}

static void assertExpressionParity(const char* key, const char* featureTag) {
  char msg[768];

  char* httpRhs = extractNormalizedRhsInRegion(kHttpFile, "doc", key, kMarkerBegin, kMarkerEnd);
  snprintf(msg, sizeof(msg),
           "EXPRESSION DE `%s` INTROUVABLE dans %s (entre les marqueurs "
           "PARITE-CONFIG, forme attendue doc[\"%s\"] = ... ;). Si la clé a "
           "changé de forme, mettre ce test à jour SCIEMMENT. NE PAS "
           "DESACTIVER CE TEST.", key, kHttpFile, key);
  TEST_ASSERT_NOT_NULL_MESSAGE(httpRhs, msg);

  char* wsRhs = extractNormalizedRhsInRegion(kWsFile, "d", key, kMarkerBegin, kMarkerEnd);
  snprintf(msg, sizeof(msg),
           "EXPRESSION DE `%s` INTROUVABLE dans %s (entre les marqueurs "
           "PARITE-CONFIG, forme attendue d[\"%s\"] = ... ;). NE PAS "
           "DESACTIVER CE TEST.", key, kWsFile, key);
  TEST_ASSERT_NOT_NULL_MESSAGE(wsRhs, msg);

  if (httpRhs != NULL && wsRhs != NULL) {
    snprintf(msg, sizeof(msg),
             "PARITE D'EXPRESSION ROMPUE sur `%s` (%s) : "
             "/get-config (%s) et la trame WS `config` (%s) ne la CALCULENT "
             "PAS pareil.\n  /get-config : %s\n  WS config   : %s\n"
             "window._config et latestConfig sont remplacés par la dernière "
             "charge reçue côté client : une divergence ferait OSCILLER la "
             "valeur affichée au rythme des deux cadences. QUE FAIRE : aligner "
             "les deux expressions. NE PAS DESACTIVER CE TEST.",
             key, featureTag, kHttpFile, kWsFile, httpRhs, wsRhs);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(httpRhs, wsRhs, msg);
  }
  free(httpRhs);
  free(wsRhs);
}

void test_h_lighting_expression_partagee_identique_des_deux_cotes(void) {
  for (size_t i = 0; i < kLightingExprKeysCount; ++i) {
    assertExpressionParity(kLightingExprKeys[i], "feature-162");
  }
}

// ---------------------------------------------------------------------------
// (i) feature-164 (gate pool-chemistry, 2e passage) : PARITE D'EXPRESSION pour
// ph_target / orp_target. 6e occurrence documentée du piège (même clé des
// deux côtés, expressions divergentes) — `ph_target` servait
// `roundf(mqttCfg.phTarget * 100.0f) / 100.0f` côté HTTP contre
// `mqttCfg.phTarget` brut côté WS, corrigé par ce lot. Sentinelle ajoutée pour
// qu'une 7e occurrence ne soit plus possible SANS faire échouer ce test.
// ---------------------------------------------------------------------------
static const char* kRegulationTargetExprKeys[] = {
  "ph_target", "orp_target",
};
static const size_t kRegulationTargetExprKeysCount =
    sizeof(kRegulationTargetExprKeys) / sizeof(kRegulationTargetExprKeys[0]);

void test_i_regulation_target_expression_partagee_identique_des_deux_cotes(void) {
  for (size_t i = 0; i < kRegulationTargetExprKeysCount; ++i) {
    assertExpressionParity(kRegulationTargetExprKeys[i], "feature-164");
  }
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
  RUN_TEST(test_g_orp_enabled_derive_jamais_ecrit_directement);
  RUN_TEST(test_h_lighting_expression_partagee_identique_des_deux_cotes);
  RUN_TEST(test_i_regulation_target_expression_partagee_identique_des_deux_cotes);
  return UNITY_END();
}
