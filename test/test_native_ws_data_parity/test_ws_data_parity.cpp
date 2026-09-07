// =============================================================================
// SENTINELLE DE PARITÉ — GET /data  ↔  trame WebSocket `sensor_data`  (feature-092)
// =============================================================================
// SECOND AXE de la garde structurelle posée par bug-089. Le premier axe
// (test_native_ws_config_parity) surveille /get-config ↔ trame `config` ; celui-ci
// surveille l'axe des MESURES, qui est le plus exposé des deux : la trame
// `sensor_data` est poussée toutes les ~5 s, la charge /data toutes les ~30 s, et
// `latestSensorData` est REMPLACÉ en entier par la dernière des deux
// (data/app.js, loadSensorData : `latestSensorData = json`, aucune fusion).
//
// CONSÉQUENCE, ET C'EST LE PIÈGE N° 1 DU PROJET (3 récidives en 3 versions) :
// un champ servi d'un SEUL côté ne disparaît pas, il CLIGNOTE. Le remplacement
// étant total, le sens de l'écart n'a AUCUNE importance ici — contrairement à
// l'axe config, où seul le sens « WS-seul » était dangereux. Les deux directions
// sont donc surveillées, et les deux exigent un motif écrit.
//
// CE QU'IL VÉRIFIE
//   (a) les marqueurs d'ancrage existent des DEUX côtés, dans le bon ordre —
//       leur disparition rend le test aveugle, donc elle est un ÉCHEC EXPLICITE ;
//   (b) entre les marqueurs, les deux côtés portent EXACTEMENT le même jeu de
//       clés — c'est la convention normative écrite dans les deux fichiers ;
//   (c) toute clé de la trame `sensor_data` est SOIT dans /data, SOIT déclarée
//       dans l'une des deux tables d'exception WS AVEC un motif écrit ;
//   (d) toute clé de /data est SOIT dans la trame, SOIT déclarée /data-seule ;
//   (e) aucune exception MORTE ni CONTREDITE : une clé déclarée qui n'existe
//       plus, ou qui a rejoint l'autre côté, fait échouer le test — sans quoi
//       les tables pourrissent en décharge de justifications périmées ;
//   (f) chaque exception porte un motif de ≥ 20 caractères, sans doublon ;
//   (g) CLIQUET : le nombre d'exceptions WS-seules ne peut pas AUGMENTER.
//
// TROIS CATÉGORIES D'EXCEPTION, ET POURQUOI ELLES SONT DISTINCTES
//   1. kWsOnlyLive — état vivant servi par AUCUNE route HTTP. C'est la DETTE
//      inventoriée, antérieure à feature-092 : elle est déclarée pour rendre la
//      sentinelle utilisable, PAS pour valider le motif. Le cliquet (g) interdit
//      qu'elle grossisse : une clé neuve va dans /data, jamais dans cette table.
//   2. kWsPlusOtherRoute — clés DUAL-SOURCÉES : absentes de /data mais servies
//      par une AUTRE route HTTP (le plus souvent /get-config, via
//      `window._config`). Catégorie qui n'existe PAS sur l'axe config : une règle
//      « aucune clé WS-seule » les signalerait à tort. Le nom de la route est
//      donc OBLIGATOIRE dans la déclaration, et asserté non vide.
//   3. kDataOnly — clés servies par /data seul. Sens inverse, même effet.
//
// ANGLE MORT CONNU (identique à l'axe config) : l'extraction est TEXTUELLE. Une
// clé mise en COMMENTAIRE reste comptée comme présente. La sentinelle vise la
// distraction, pas la malveillance. Ne pas essayer de parser du C++ ici.
//
// CONVENTION NORMATIVE LUE PAR CE TEST
//   src/web_routes_data.cpp : fonction handleGetData, receveur `doc[` ;
//   src/ws_manager.cpp      : fonction _buildSensorJson, receveur `d[`.
//   Receveurs IMBRIQUÉS EXCLUS : `faObj[` (objet `flow_assist`) porte neuf
//   champs qui ne sont PAS des clés de premier niveau — les compter polluerait
//   le diff des deux côtés.
//   Marqueurs : `>>> PARITE-DATA:DEBUT` et `<<< PARITE-DATA:FIN`.
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

static const size_t kMinReasonLen = 20;

static const char* kMarkerBegin = ">>> PARITE-DATA:DEBUT";
static const char* kMarkerEnd   = "<<< PARITE-DATA:FIN";

static const char* kHttpFile = "src/web_routes_data.cpp";
static const char* kWsFile   = "src/ws_manager.cpp";

// Bornes de PORTÉE : la parité porte sur la charge /data et sur la trame
// `sensor_data`, pas sur les autres fabricants de JSON des deux fichiers
// (handleGetKpi, _buildConfigJson). Ces ancres sont des signatures de fonction :
// leur disparition est un échec explicite, comme celle d'un marqueur.
static const char* kHttpScopeBegin = "static void handleGetData(";
static const char* kHttpScopeEnd   = "static void handleGetKpi(";
static const char* kWsScopeBegin   = "size_t WsManager::_buildSensorJson(";
static const char* kWsScopeEnd     = "size_t WsManager::_buildConfigJson(";

// ---------------------------------------------------------------------------
// CATÉGORIE 1 — état vivant WS-seul, servi par AUCUNE route HTTP.
// DETTE INVENTORIÉE (antérieure à feature-092), pas un satisfecit : plusieurs de
// ces clés sont lues depuis `latestSensorData` et leur consommateur retombe donc
// sur sa valeur de repli après chaque charge /data. Le motif le dit.
// ---------------------------------------------------------------------------
struct WsOnlyKey {
  const char* key;
  const char* reason;
};

static const WsOnlyKey kWsOnlyLive[] = {
  {"ph_used_ms",
   "Etat vivant de la fenetre horaire (ms consommees, avance en continu) : aucune route HTTP ne le sert. Consomme par getDosingStopReason avec repli `|| 0` — le motif « Limite de duree atteinte » retombe apres chaque charge /data (dette anterieure a feature-092)."},
  {"orp_used_ms",
   "Etat vivant de la fenetre horaire ORP (ms consommees) : aucune route HTTP ne le sert, meme repli `|| 0` que ph_used_ms dans getDosingStopReason (dette anterieure a feature-092)."},
  {"ph_stab_remaining_s",
   "Compte a rebours de stabilisation PAR POMPE (feature-012 N3), decroit a la seconde : aucune route HTTP ne le sert, le client retombe sur stabilization_remaining_s."},
  {"orp_stab_remaining_s",
   "Compte a rebours de stabilisation ORP par pompe, decroit a la seconde : aucune route HTTP ne le sert, meme repli que la voie pH."},
  {"stabilization_remaining_s",
   "Compte a rebours GLOBAL de stabilisation (max des deux voies), repli de compatibilite pour firmware ancien : aucune route HTTP ne le sert."},
  {"ph_scheduled_flow_ml_per_min",
   "Debit calcule du mode Programmee (feature-011), recalcule a chaque push : aucune route HTTP ne le sert. La ligne affiche « — » apres chaque charge /data — dette anterieure a feature-092, a traiter par une spec dediee."},
  {"orp_scheduled_flow_ml_per_min",
   "Debit calcule du mode Programmee ORP : aucune route HTTP ne le sert, meme degradation en « — » que la voie pH apres une charge /data."},
  {"temperature_raw",
   "Temperature BRUTE sans offset, servie pour l'ecran de calibration uniquement : aucune route HTTP ne l'expose, la calibration se fait a l'ecoute des pushes WS."},
  {"reset_reason",
   "Cause du dernier redemarrage, consommee A LA RECEPTION de la trame (toast de redemarrage, app.js l.144) et jamais relue depuis latestSensorData : la charge /data ne peut donc rien effacer."},
  {"filtration_state_source",
   "Diagnostic d'origine du verdict de filtration (« interne » / « externe ») : aucune route HTTP ne le sert et AUCUN consommateur dans app.js — trace de journal, pas d'affichage."},
  {"filtration_state_stale",
   "Peremption du signal de filtration, evaluee a chaque push : aucune route HTTP ne le sert. Consomme avec la garde `=== true`, donc degradation silencieuse (« Filtration arretee » au lieu de « Signal perime ») apres une charge /data."},
  {"filtration_ext_known",
   "Presence d'un verdict de filtration EXTERNE, etat vivant du lien : aucune route HTTP ne le sert, garde `=== true` cote client."},
  {"filtration_ext_on",
   "Verdict de filtration externe (marche/arret) : aucune route HTTP ne le sert et aucun consommateur dans app.js — diagnostic de trame."},
  {"filtration_ext_age_s",
   "Age du dernier verdict externe, avance a la seconde : aucune route HTTP ne le sert, garde `Number.isFinite` cote client."},
  {"boost_filtration_extended",
   "Detail de composition du boost en cours, mis a jour par la reponse de commande (app.js l.5312 ecrit directement dans latestSensorData) : aucune route HTTP ne le sert."},
  {"boost_chlorine_boosted",
   "Detail de composition du boost en cours (volet chlore), meme mecanique d'ecriture directe par la reponse de commande : aucune route HTTP ne le sert."},
  {"filtration_force_on",
   "Echo de la commande de forcage marche : ECRIT par le client (POST /set-config) et jamais relu — aucune route HTTP ne le renvoie, la trame sert de confirmation d'etat."},
  {"filtration_force_off",
   "Echo de la commande de forcage arret : ecrit par le client (POST /set-config) et jamais relu, meme mecanique que filtration_force_on."},
};
static const size_t kWsOnlyLiveCount = sizeof(kWsOnlyLive) / sizeof(kWsOnlyLive[0]);

// CLIQUET (g) : borne de l'état constaté le 2026-09-07, feature-092. Le compte
// ne doit JAMAIS augmenter — une clé neuve de la trame va dans /data.
static const size_t kWsOnlyLiveCeiling = 18;

// ---------------------------------------------------------------------------
// CATÉGORIE 2 — clés DUAL-SOURCÉES : absentes de /data, mais servies par une
// autre route HTTP. Le nom de la route est OBLIGATOIRE (asserté non vide) : sans
// lui, l'exception est indiscernable d'un vrai trou de parité.
// ---------------------------------------------------------------------------
struct WsPlusRouteKey {
  const char* key;
  const char* route;
  const char* reason;
};

static const WsPlusRouteKey kWsPlusOtherRoute[] = {
  {"ph_container_ml", "GET /get-config",
   "Volume du bidon pH : REGLAGE, servi par /get-config vers window._config ; la trame le pousse pour la jauge de niveau qui, elle, bouge a l'usage."},
  {"orp_container_ml", "GET /get-config",
   "Volume du bidon ORP : meme partage que la voie pH — reglage cote /get-config, valeur vivante cote trame pour la jauge de niveau."},
  {"lighting_enabled", "GET /get-config",
   "Etat de l'eclairage : REGLAGE persistant cote /get-config, et etat instantane cote trame (le planificateur peut le basculer sans requete HTTP)."},
  {"boost_active", "GET /get-config",
   "Boost en cours : derive du temps cote /get-config (isBoostActive) et pousse en continu par la trame pour l'extinction automatique du bandeau."},
  {"boost_until", "GET /get-config",
   "Echeance du boost (epoch) : servie par /get-config et repoussee par la trame pour le compte a rebours affiche sans requete."},
  {"install_mode", "GET /get-config",
   "Mode d'installation (feature-056) : REGLAGE servi par /get-config ; la trame le porte pour que les vues pilotees par le mode se rendent sans attendre la config."},
  {"mqtt_connected", "GET /get-config",
   "Etat de la liaison MQTT : servi par /get-config au chargement, puis rafraichi par la trame — le consommateur ecrit d'ailleurs dans window._config (app.js l.138)."},
  {"uptime_ms", "GET /logs",
   "Duree de fonctionnement : servie par /logs (web_routes_data.cpp l.860) pour datter les lignes de journal, et poussee par la trame pour l'ancrage d'horloge du client."},
};
static const size_t kWsPlusOtherRouteCount =
    sizeof(kWsPlusOtherRoute) / sizeof(kWsPlusOtherRoute[0]);

// ---------------------------------------------------------------------------
// CATÉGORIE 3 — clés servies par /data SEUL.
// ---------------------------------------------------------------------------
static const WsOnlyKey kDataOnly[] = {
  {"ph_pump_amplitude_duty",
   "Rapport cyclique d'amplitude de la pompe pH : grandeur de CALIBRATION, lue dans les vues de diagnostic servies par /data — jamais un affichage temps reel."},
  {"orp_pump_amplitude_duty",
   "Rapport cyclique d'amplitude de la pompe ORP : meme grandeur de calibration, meme usage de diagnostic que la voie pH."},
  {"flow_assist",
   "RECEVEUR IMBRIQUE de l'assistant de debit (feature-075) : neuf champs sous un objet, servis par /data pendant une session d'assistance seulement. Ses champs internes sont ecrits via `faObj[` et doivent rester HORS du diff de premier niveau."},
};
static const size_t kDataOnlyCount = sizeof(kDataOnly) / sizeof(kDataOnly[0]);

// ===========================================================================
// Lecture d'un fichier du dépôt — même helper que l'axe config : racine
// reconstruite depuis __FILE__, avec repli sur des préfixes relatifs.
// ===========================================================================
static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  const char* candidates[5];
  char rooted[512];

  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_ws_data_parity");
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

// Extrait les clés `<recv>["<clé>"]` de la région [from ; to[.
static void collectKeys(const char* from, const char* to, const char* fileStart,
                        const char* recv, KeySet& out) {
  char needle[32];
  snprintf(needle, sizeof(needle), "%s[\"", recv);
  const size_t nl = strlen(needle);

  for (const char* p = from; p + nl < to; ++p) {
    // Le receveur doit être un identifiant COMPLET : sinon `faObj["active"]`
    // serait capté par le receveur `d` (et `bySize["small"]` par `doc`). C'est
    // exactement l'exclusion des receveurs imbriqués exigée par la convention.
    if (p > fileStart) {
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
}

// Codes d'erreur : -1 marqueur DEBUT absent, -2 marqueur FIN absent,
// -3 fichier introuvable, -4 marqueurs inversés, -5 ancre de PORTÉE absente,
// -6 marqueur hors de la portée. >= 0 : nombre de clés extraites.
static int extractScopeKeys(const char* rel, const char* recv,
                            const char* scopeBegin, const char* scopeEnd,
                            KeySet& outScope, KeySet& outMarked) {
  outScope.count = 0;
  outMarked.count = 0;
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src == NULL) return -3;

  const char* s = strstr(src, scopeBegin);
  if (s == NULL) { free(src); return -5; }
  const char* e = strstr(s, scopeEnd);
  if (e == NULL) { free(src); return -5; }

  const char* b = strstr(s, kMarkerBegin);
  const char* f = strstr(s, kMarkerEnd);
  if (b == NULL || b > e) { free(src); return (b == NULL) ? -1 : -6; }
  if (f == NULL || f > e) { free(src); return (f == NULL) ? -2 : -6; }
  if (f < b)              { free(src); return -4; }

  collectKeys(s, e, src, recv, outScope);
  collectKeys(b, f, src, recv, outMarked);
  free(src);
  return (int)outScope.count;
}

static void failOnExtractError(const char* rel, const char* recv, int rc) {
  char msg[640];
  const char* what =
    rc == -1 ? "marqueur de DEBUT « >>> PARITE-DATA:DEBUT » ABSENT" :
    rc == -2 ? "marqueur de FIN « <<< PARITE-DATA:FIN » ABSENT" :
    rc == -3 ? "fichier source INTROUVABLE" :
    rc == -4 ? "marqueurs INVERSES (FIN avant DEBUT)" :
    rc == -5 ? "ancre de PORTEE (signature de fonction) INTROUVABLE" :
    rc == -6 ? "marqueur situe HORS de la portee surveillee" : "erreur inconnue";
  snprintf(msg, sizeof(msg),
           "SENTINELLE AVEUGLE dans %s : %s. Sans les ancres, aucune cle n'est "
           "comparee et la parite /data <-> trame `sensor_data` cesse d'etre "
           "garantie — or un champ servi d'un seul cote ne disparait pas, il "
           "CLIGNOTE (latestSensorData est REMPLACE en entier par la derniere "
           "charge recue). QUE FAIRE : RESTAURER le marqueur autour du bloc de "
           "cles partagees (receveur `%s[`), ou corriger l'ancre de portee dans "
           "ce test si la fonction a ete renommee — NE PAS DESACTIVER CE TEST.",
           rel, what, recv);
  TEST_FAIL_MESSAGE(msg);
}

// Chargement commun ; renvoie false si un échec a déjà été signalé.
static bool loadBothSides(KeySet& http, KeySet& httpMarked,
                          KeySet& ws, KeySet& wsMarked) {
  const int rcHttp = extractScopeKeys(kHttpFile, "doc", kHttpScopeBegin,
                                      kHttpScopeEnd, http, httpMarked);
  if (rcHttp < 0) { failOnExtractError(kHttpFile, "doc", rcHttp); return false; }
  const int rcWs = extractScopeKeys(kWsFile, "d", kWsScopeBegin, kWsScopeEnd,
                                    ws, wsMarked);
  if (rcWs < 0) { failOnExtractError(kWsFile, "d", rcWs); return false; }
  return true;
}

static bool declaredWsOnly(const char* k) {
  for (size_t i = 0; i < kWsOnlyLiveCount; ++i) {
    if (strcmp(kWsOnlyLive[i].key, k) == 0) return true;
  }
  for (size_t i = 0; i < kWsPlusOtherRouteCount; ++i) {
    if (strcmp(kWsPlusOtherRoute[i].key, k) == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// (a) Ancres et marqueurs présents des deux côtés, extraction non vide.
// ---------------------------------------------------------------------------
void test_a_ancres_et_marqueurs_presents_des_deux_cotes(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  char msg[512];
  snprintf(msg, sizeof(msg),
           "Extraction SUSPECTE : seulement %u cle(s) `doc[\"...\"]` dans "
           "handleGetData (%s). Un extracteur qui ne trouve rien ferait PASSER "
           "toutes les autres assertions. QUE FAIRE : verifier la convention "
           "(receveur `doc[`) et les ancres de portee — NE PAS DESACTIVER CE "
           "TEST.", (unsigned)http.count, kHttpFile);
  TEST_ASSERT_TRUE_MESSAGE(http.count >= 80, msg);

  snprintf(msg, sizeof(msg),
           "Extraction SUSPECTE : seulement %u cle(s) `d[\"...\"]` dans "
           "_buildSensorJson (%s). QUE FAIRE : verifier la convention "
           "(receveur `d[`) et les ancres de portee — NE PAS DESACTIVER CE "
           "TEST.", (unsigned)ws.count, kWsFile);
  TEST_ASSERT_TRUE_MESSAGE(ws.count >= 100, msg);

  snprintf(msg, sizeof(msg),
           "BLOC DE MARQUEURS VIDE : %u cle(s) entre les marqueurs cote /data, "
           "%u cote WS. Les marqueurs encadrent le mauvais bloc. QUE FAIRE : "
           "les replacer autour des cles partagees de feature-092 — NE PAS "
           "DESACTIVER CE TEST.",
           (unsigned)httpMarked.count, (unsigned)wsMarked.count);
  TEST_ASSERT_TRUE_MESSAGE(httpMarked.count >= 14, msg);
  TEST_ASSERT_TRUE_MESSAGE(wsMarked.count >= 14, msg);
}

// ---------------------------------------------------------------------------
// (b) Entre les marqueurs : MÊME jeu de clés des deux côtés. C'est la
// convention normative écrite dans les deux fichiers, verifiée dans les deux
// sens (aucune clé orpheline ni d'un côté ni de l'autre).
// ---------------------------------------------------------------------------
void test_b_le_bloc_marque_porte_les_memes_cles_des_deux_cotes(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  for (size_t i = 0; i < httpMarked.count; ++i) {
    char msg[640];
    snprintf(msg, sizeof(msg),
             "BLOC MARQUE DESYNCHRONISE : « %s » est entre les marqueurs de %s "
             "mais PAS entre ceux de %s. La convention normative ecrite dans "
             "les deux fichiers exige les MEMES cles entre les MEMES marqueurs, "
             "servies dans le MEME passage. QUE FAIRE : ajouter la cle de "
             "l'autre cote, entre les marqueurs, avec la MEME orthographe — NE "
             "PAS DESACTIVER CE TEST.",
             httpMarked.keys[i], kHttpFile, kWsFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(wsMarked, httpMarked.keys[i]), msg);
  }
  for (size_t i = 0; i < wsMarked.count; ++i) {
    char msg[640];
    snprintf(msg, sizeof(msg),
             "BLOC MARQUE DESYNCHRONISE : « %s » est entre les marqueurs de %s "
             "mais PAS entre ceux de %s. QUE FAIRE : ajouter la cle cote /data, "
             "entre les marqueurs, avec la MEME orthographe — NE PAS "
             "DESACTIVER CE TEST.",
             wsMarked.keys[i], kWsFile, kHttpFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(httpMarked, wsMarked.keys[i]), msg);
  }
}

// ---------------------------------------------------------------------------
// (c) Aucune clé WS non couverte : dans /data, ou déclarée (cat. 1 ou 2).
// ---------------------------------------------------------------------------
void test_c_aucune_cle_ws_seule_non_declaree(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  for (size_t i = 0; i < ws.count; ++i) {
    const char* k = ws.keys[i];
    if (keySetHas(http, k)) continue;
    char msg[900];
    snprintf(msg, sizeof(msg),
             "PARITE ROMPUE : la cle « %s » est poussee par la trame "
             "`sensor_data` (%s) mais ABSENTE de GET /data (%s) et NON "
             "DECLAREE. Elle ne manquera pas : elle CLIGNOTERA — "
             "latestSensorData est remplace en entier par la charge /data "
             "toutes les ~30 s, puis remis par le push WS. QUE FAIRE — au "
             "choix : (1) AJOUTER la cle a handleGetData, ENTRE LES MARQUEURS "
             "(receveur `doc[`), avec la MEME orthographe — c'est la reponse "
             "par defaut ; (2) si elle est servie par une AUTRE route HTTP, "
             "l'inscrire dans kWsPlusOtherRoute avec le NOM de cette route ; "
             "(3) exceptionnellement, dans kWsOnlyLive avec un motif de >= %u "
             "caracteres — mais le cliquet du test (g) interdit d'agrandir "
             "cette table. NE PAS DESACTIVER CE TEST.",
             k, kWsFile, kHttpFile, (unsigned)kMinReasonLen);
    TEST_ASSERT_TRUE_MESSAGE(declaredWsOnly(k), msg);
  }
}

// ---------------------------------------------------------------------------
// (d) Aucune clé /data-seule non déclarée — sens inverse, même effet.
// ---------------------------------------------------------------------------
void test_d_aucune_cle_data_seule_non_declaree(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  for (size_t i = 0; i < http.count; ++i) {
    const char* k = http.keys[i];
    if (keySetHas(ws, k)) continue;
    bool declared = false;
    for (size_t j = 0; j < kDataOnlyCount; ++j) {
      if (strcmp(kDataOnly[j].key, k) == 0) { declared = true; break; }
    }
    char msg[900];
    snprintf(msg, sizeof(msg),
             "PARITE ROMPUE (sens /data) : la cle « %s » est servie par GET "
             "/data (%s) mais ABSENTE de la trame `sensor_data` (%s) et NON "
             "DECLAREE. Le remplacement de latestSensorData etant TOTAL, ce "
             "sens clignote aussi : le champ disparait a chaque push WS. QUE "
             "FAIRE — au choix : (1) AJOUTER la cle a _buildSensorJson, ENTRE "
             "LES MARQUEURS (receveur `d[`), en verifiant le budget (plafond "
             "DUR kWsFrameBufBytes = 3584 o, un depassement SAUTE le push) ; "
             "OU (2) L'INSCRIRE dans kDataOnly avec un motif de >= %u "
             "caracteres. NE PAS DESACTIVER CE TEST.",
             k, kHttpFile, kWsFile, (unsigned)kMinReasonLen);
    TEST_ASSERT_TRUE_MESSAGE(declared, msg);
  }
}

// ---------------------------------------------------------------------------
// (e) Aucune exception MORTE ni CONTREDITE, dans les trois tables.
// ---------------------------------------------------------------------------
void test_e_aucune_exception_morte_ni_contredite(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  char msg[768];

  for (size_t j = 0; j < kWsOnlyLiveCount; ++j) {
    const char* k = kWsOnlyLive[j].key;
    snprintf(msg, sizeof(msg),
             "EXCEPTION MORTE : « %s » est declaree WS-seule dans kWsOnlyLive, "
             "mais cette cle n'est PLUS poussee par la trame `sensor_data` "
             "(%s). Une exception qui ne protege plus rien transforme la table "
             "en decharge de justifications perimees et masquera la prochaine "
             "vraie divergence. QUE FAIRE : SUPPRIMER l'entree (et baisser le "
             "cliquet kWsOnlyLiveCeiling d'autant), ou restaurer la cle si son "
             "retrait est un accident. NE PAS DESACTIVER CE TEST.", k, kWsFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(ws, k), msg);

    snprintf(msg, sizeof(msg),
             "EXCEPTION CONTREDITE : « %s » est declaree WS-seule alors qu'elle "
             "EST desormais servie par GET /data (%s). Le motif ecrit ne decrit "
             "plus le code. QUE FAIRE : SUPPRIMER l'entree de kWsOnlyLive (la "
             "parite est atteinte pour cette cle) et baisser le cliquet. NE PAS "
             "DESACTIVER CE TEST.", k, kHttpFile);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(http, k), msg);
  }

  for (size_t j = 0; j < kWsPlusOtherRouteCount; ++j) {
    const char* k = kWsPlusOtherRoute[j].key;
    snprintf(msg, sizeof(msg),
             "EXCEPTION MORTE : « %s » est declaree dual-sourcee dans "
             "kWsPlusOtherRoute, mais elle n'est PLUS poussee par la trame "
             "`sensor_data` (%s). QUE FAIRE : SUPPRIMER l'entree, ou restaurer "
             "la cle. NE PAS DESACTIVER CE TEST.", k, kWsFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(ws, k), msg);

    snprintf(msg, sizeof(msg),
             "EXCEPTION CONTREDITE : « %s » est declaree dual-sourcee (servie "
             "par %s) alors qu'elle EST desormais servie par GET /data lui-meme "
             "(%s). QUE FAIRE : SUPPRIMER l'entree de kWsPlusOtherRoute. NE PAS "
             "DESACTIVER CE TEST.", k, kWsPlusOtherRoute[j].route, kHttpFile);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(http, k), msg);
  }

  for (size_t j = 0; j < kDataOnlyCount; ++j) {
    const char* k = kDataOnly[j].key;
    snprintf(msg, sizeof(msg),
             "EXCEPTION MORTE : « %s » est declaree /data-seule dans kDataOnly, "
             "mais cette cle n'est PLUS servie par GET /data (%s). QUE FAIRE : "
             "SUPPRIMER l'entree, ou restaurer la cle si son retrait est un "
             "accident. NE PAS DESACTIVER CE TEST.", k, kHttpFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(http, k), msg);

    snprintf(msg, sizeof(msg),
             "EXCEPTION CONTREDITE : « %s » est declaree /data-seule alors "
             "qu'elle EST desormais poussee par la trame `sensor_data` (%s). QUE "
             "FAIRE : SUPPRIMER l'entree de kDataOnly (la parite est atteinte). "
             "NE PAS DESACTIVER CE TEST.", k, kWsFile);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(ws, k), msg);
  }
}

// ---------------------------------------------------------------------------
// (f) Motif obligatoire, route obligatoire, aucun doublon.
// ---------------------------------------------------------------------------
void test_f_chaque_exception_porte_un_motif_ecrit(void) {
  char msg[640];

  for (size_t j = 0; j < kWsOnlyLiveCount; ++j) {
    const char* k = kWsOnlyLive[j].key;
    const char* r = kWsOnlyLive[j].reason;
    snprintf(msg, sizeof(msg),
             "EXCEPTION SANS MOTIF : « %s » (kWsOnlyLive) n'a pas de "
             "justification ecrite d'au moins %u caracteres. QUE FAIRE : ecrire "
             "POURQUOI aucune route HTTP ne sert cette cle, ET quelle "
             "degradation le client subit apres une charge /data. NE PAS "
             "DESACTIVER CE TEST.", k, (unsigned)kMinReasonLen);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, msg);
    TEST_ASSERT_TRUE_MESSAGE(strlen(r) >= kMinReasonLen, msg);
  }

  for (size_t j = 0; j < kWsPlusOtherRouteCount; ++j) {
    const char* k = kWsPlusOtherRoute[j].key;
    const char* r = kWsPlusOtherRoute[j].reason;
    const char* rt = kWsPlusOtherRoute[j].route;
    snprintf(msg, sizeof(msg),
             "EXCEPTION SANS MOTIF : « %s » (kWsPlusOtherRoute) n'a pas de "
             "justification ecrite d'au moins %u caracteres.", k,
             (unsigned)kMinReasonLen);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, msg);
    TEST_ASSERT_TRUE_MESSAGE(strlen(r) >= kMinReasonLen, msg);

    snprintf(msg, sizeof(msg),
             "EXCEPTION SANS ROUTE : « %s » est declaree dual-sourcee sans nom "
             "de route HTTP. Sans lui, l'exception est indiscernable d'un vrai "
             "trou de parite. QUE FAIRE : nommer la route qui sert la cle "
             "(ex. « GET /get-config »). NE PAS DESACTIVER CE TEST.", k);
    TEST_ASSERT_NOT_NULL_MESSAGE(rt, msg);
    TEST_ASSERT_TRUE_MESSAGE(strlen(rt) >= 5, msg);
  }

  for (size_t j = 0; j < kDataOnlyCount; ++j) {
    const char* k = kDataOnly[j].key;
    const char* r = kDataOnly[j].reason;
    snprintf(msg, sizeof(msg),
             "EXCEPTION SANS MOTIF : « %s » (kDataOnly) n'a pas de "
             "justification ecrite d'au moins %u caracteres.", k,
             (unsigned)kMinReasonLen);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, msg);
    TEST_ASSERT_TRUE_MESSAGE(strlen(r) >= kMinReasonLen, msg);
  }

  // Doublons — dans une table comme entre les tables : deux motifs pour une
  // meme cle, c'est un des deux qui sera ignoré.
  const char* all[kMaxKeys];
  size_t n = 0;
  for (size_t j = 0; j < kWsOnlyLiveCount; ++j)       all[n++] = kWsOnlyLive[j].key;
  for (size_t j = 0; j < kWsPlusOtherRouteCount; ++j) all[n++] = kWsPlusOtherRoute[j].key;
  for (size_t j = 0; j < kDataOnlyCount; ++j)         all[n++] = kDataOnly[j].key;
  for (size_t a = 0; a < n; ++a) {
    for (size_t b = a + 1; b < n; ++b) {
      snprintf(msg, sizeof(msg),
               "EXCEPTION EN DOUBLE : « %s » apparait deux fois dans les tables "
               "d'exception. QUE FAIRE : fusionner en une seule entree, avec le "
               "motif le plus complet.", all[a]);
      TEST_ASSERT_FALSE_MESSAGE(strcmp(all[a], all[b]) == 0, msg);
    }
  }
}

// ---------------------------------------------------------------------------
// (g) CLIQUET + inventaire cohérent. Le nombre d'exceptions WS-seules
// « etat vivant » ne peut pas augmenter, et le decompte constate doit
// correspondre EXACTEMENT a la somme des deux tables WS.
// ---------------------------------------------------------------------------
void test_g_cliquet_et_inventaire_coherent(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  char msg[768];
  snprintf(msg, sizeof(msg),
           "CLIQUET FRANCHI : %u entree(s) dans kWsOnlyLive pour un plafond de "
           "%u. Cette table est une DETTE inventoriee, pas un motif "
           "acceptable : une cle NEUVE de la trame `sensor_data` doit etre "
           "ajoutee a GET /data (entre les marqueurs), pas declaree ici. QUE "
           "FAIRE : servir la cle des deux cotes. NE PAS relever le plafond.",
           (unsigned)kWsOnlyLiveCount, (unsigned)kWsOnlyLiveCeiling);
  TEST_ASSERT_TRUE_MESSAGE(kWsOnlyLiveCount <= kWsOnlyLiveCeiling, msg);

  size_t wsOnly = 0, dataOnly = 0;
  for (size_t i = 0; i < ws.count; ++i)   if (!keySetHas(http, ws.keys[i]))  wsOnly++;
  for (size_t i = 0; i < http.count; ++i) if (!keySetHas(ws, http.keys[i]))  dataOnly++;

  snprintf(msg, sizeof(msg),
           "INVENTAIRE INCOHERENT : %u cle(s) WS-seule(s) constatee(s) dans le "
           "code pour %u entree(s) declarees (%u « etat vivant » + %u "
           "dual-sourcees). QUE FAIRE : servir la cle manquante des deux cotes, "
           "OU la declarer dans la bonne table ; si une exception est devenue "
           "inutile, la supprimer. NE PAS DESACTIVER CE TEST.",
           (unsigned)wsOnly,
           (unsigned)(kWsOnlyLiveCount + kWsPlusOtherRouteCount),
           (unsigned)kWsOnlyLiveCount, (unsigned)kWsPlusOtherRouteCount);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(kWsOnlyLiveCount + kWsPlusOtherRouteCount,
                                   wsOnly, msg);

  snprintf(msg, sizeof(msg),
           "INVENTAIRE INCOHERENT : %u cle(s) /data-seule(s) constatee(s) pour "
           "%u entree(s) dans kDataOnly. QUE FAIRE : ajouter la cle a la trame "
           "OU la declarer avec un motif ; supprimer les exceptions devenues "
           "inutiles. NE PAS DESACTIVER CE TEST.",
           (unsigned)dataOnly, (unsigned)kDataOnlyCount);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(kDataOnlyCount, dataOnly, msg);
}

// ---------------------------------------------------------------------------
// (h) EXCLUSION DES RECEVEURS IMBRIQUÉS : les neuf champs de `flow_assist` sont
// écrits via `faObj[` et ne doivent JAMAIS entrer dans le diff — sinon ils
// apparaitraient comme neuf fausses cles /data-seules.
// ---------------------------------------------------------------------------
void test_h_les_receveurs_imbriques_sont_exclus_du_diff(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  // Champs internes de `flow_assist` (receveur `faObj[`).
  const char* nested[] = {"active", "pump", "phase", "remaining_s", "aborted",
                          "purges_left", "purge_s", "measure_s", "flow255"};
  for (size_t i = 0; i < sizeof(nested) / sizeof(nested[0]); ++i) {
    char msg[640];
    snprintf(msg, sizeof(msg),
             "RECEVEUR IMBRIQUE CAPTE : « %s » est un champ de l'objet "
             "`flow_assist` (ecrit via `faObj[`), il a ete compte comme une cle "
             "de PREMIER NIVEAU de /data. Le diff se remplirait de neuf fausses "
             "cles /data-seules et la sentinelle deviendrait illisible. QUE "
             "FAIRE : restaurer la contrainte d'identifiant COMPLET sur le "
             "receveur dans collectKeys(). NE PAS DESACTIVER CE TEST.",
             nested[i]);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(http, nested[i]), msg);
  }

  // Le receveur du document (`doc["data"]`, `doc["type"]`) de la trame WS ne
  // doit pas non plus polluer l'inventaire WS : la trame se lit sous `d[`.
  TEST_ASSERT_FALSE_MESSAGE(keySetHas(ws, "type"),
      "La cle d'enveloppe `type` (receveur `doc[`) ne doit pas entrer dans "
      "l'inventaire de la trame, qui se lit sous `d[`.");
}

// ---------------------------------------------------------------------------
// (i) ANCRAGE DE CE CYCLE : les quatre champs de PAUSE MELANGE ont rejoint
// /data (feature-092) et les huit champs de composition sont servis des deux
// cotes. C'est le contenu de la carte « Debit d'injection » : s'il repasse
// WS-seul, la pastille se remet a clignoter — exactement le defaut du 03/09.
// ---------------------------------------------------------------------------
void test_i_les_champs_de_la_carte_debit_sont_servis_des_deux_cotes(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;

  const char* shared[] = {
    // Pause mélange — WS-seuls jusqu'à feature-092 (piège n° 1 documenté).
    "phMixingDelayActive", "ph_mix_remaining_s",
    "orpMixingDelayActive", "orp_mix_remaining_s",
    // Composition du débit d'injection.
    "ph_ff_learned_ml_per_min", "ph_p_term_ml_per_min", "ph_i_term_ml_per_min",
    "ph_demand_ml_per_min", "orp_p_term_ml_per_min", "orp_demand_ml_per_min",
    "orp_sustainable_ml_per_min", "orp_limited_by_hourly_budget",
    // Qualification de FIABILITE des debits : `/data`-seules jusqu'au
    // 2026-09-07, ou CETTE SENTINELLE a fait echouer le pied de carte
    // « pompe non caracterisee » (cf. Notes techniques de feature-092).
    "ph_pump_flow_uncharacterized", "orp_pump_flow_uncharacterized",
  };
  for (size_t i = 0; i < sizeof(shared) / sizeof(shared[0]); ++i) {
    const char* k = shared[i];
    char msg[640];
    snprintf(msg, sizeof(msg),
             "CHAMP DE LA CARTE « Debit d'injection » PERDU : « %s » doit etre "
             "servi par GET /data ET par la trame `sensor_data`, ENTRE LES "
             "MARQUEURS des deux cotes. Sans la parite, la pastille d'etat "
             "clignote entre « Pause melange · N min » et « En attente » a "
             "chaque charge /data. NE PAS DESACTIVER CE TEST.", k);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(http, k), msg);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(ws, k), msg);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(httpMarked, k), msg);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(wsMarked, k), msg);
  }
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_ancres_et_marqueurs_presents_des_deux_cotes);
  RUN_TEST(test_b_le_bloc_marque_porte_les_memes_cles_des_deux_cotes);
  RUN_TEST(test_c_aucune_cle_ws_seule_non_declaree);
  RUN_TEST(test_d_aucune_cle_data_seule_non_declaree);
  RUN_TEST(test_e_aucune_exception_morte_ni_contredite);
  RUN_TEST(test_f_chaque_exception_porte_un_motif_ecrit);
  RUN_TEST(test_g_cliquet_et_inventaire_coherent);
  RUN_TEST(test_h_les_receveurs_imbriques_sont_exclus_du_diff);
  RUN_TEST(test_i_les_champs_de_la_carte_debit_sont_servis_des_deux_cotes);
  return UNITY_END();
}
