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
// SECOND AXE, IMBRIQUÉ (bug-082) — `flow_assist` est devenu BILATÉRAL, mais son
// CONTENU ne l'est pas : la trame ne pousse que deux de ses neuf champs. La
// parité de premier niveau est donc devenue AVEUGLE à l'intérieur de l'objet, et
// c'est précisément là que se joue le garde-fou : si `aborted` disparaît de la
// trame, il redevient MORT en pratique (latestSensorData écrasé avant la saisie
// du volume) et une mesure de débit avortée repart vers le calcul de dosage —
// débit sous-estimé, donc pouls ALLONGÉ *et* dailyInjectedMl sous-compté (le
// raisonnement complet est dans src/ws_manager.cpp, bloc `flow_assist`).
// Les cas (j) à (n) diffent donc le CONTENU du receveur `faObj[` des deux côtés,
// avec UN SEUL extracteur — c'est pour cela que le firmware porte le MÊME nom de
// receveur dans les deux fichiers. Table d'exception : kFlowAssistDataOnly, sept
// champs de session, motif obligatoire et cliquet, comme l'axe de premier niveau.
//
// QUATRE CATÉGORIES D'EXCEPTION, ET POURQUOI ELLES SONT DISTINCTES
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
//   4. kFlowAssistDataOnly — champs IMBRIQUÉS de `flow_assist` servis par /data
//      seul (second axe). Table à part parce que le diff est à part : ces
//      champs ne sont PAS des clés de premier niveau. Cliquet propre, et deux
//      champs (`aborted`, `flow255`) y sont formellement INTERDITS.
//
// ANGLE MORT CONNU (identique à l'axe config) : l'extraction est TEXTUELLE. Une
// clé mise en COMMENTAIRE reste comptée comme présente. La sentinelle vise la
// distraction, pas la malveillance. Ne pas essayer de parser du C++ ici.
//
// CONVENTION NORMATIVE LUE PAR CE TEST
//   src/web_routes_data.cpp : fonction handleGetData, receveur `doc[` ;
//   src/ws_manager.cpp      : fonction _buildSensorJson, receveur `d[`.
//   Receveurs IMBRIQUÉS EXCLUS DU DIFF DE PREMIER NIVEAU : `faObj[` (objet
//   `flow_assist`) porte des champs qui ne sont PAS des clés de premier niveau —
//   les compter polluerait le diff des deux côtés. Ils ne sont pas pour autant
//   dispensés de parité : ils ont leur PROPRE axe, ci-dessous.
//   Marqueurs : `>>> PARITE-DATA:DEBUT` et `<<< PARITE-DATA:FIN`.
//
// LE PIRE CAS DE CETTE SENTINELLE, C'EST SA DÉSACTIVATION. Chaque message
// d'échec dit donc QUOI FAIRE, et jamais « ignorer ce test ».
// =============================================================================

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
  {"filtration_state_stale",
   "Peremption du signal de filtration, evaluee a chaque push : aucune route HTTP ne le sert. Consomme avec la garde `=== true`, donc degradation silencieuse (« Filtration arretee » au lieu de « Signal perime ») apres une charge /data."},
  {"filtration_ext_known",
   "Presence d'un verdict de filtration EXTERNE, etat vivant du lien : aucune route HTTP ne le sert, garde `=== true` cote client."},
  {"filtration_ext_age_s",
   "Age du dernier verdict externe, avance a la seconde : aucune route HTTP ne le sert, garde `Number.isFinite` cote client."},
  {"boost_filtration_extended",
   "Detail de composition du boost en cours, mis a jour par la reponse de commande (app.js l.5312 ecrit directement dans latestSensorData) : aucune route HTTP ne le sert."},
  {"boost_chlorine_boosted",
   "Detail de composition du boost en cours (volet chlore), meme mecanique d'ecriture directe par la reponse de commande : aucune route HTTP ne le sert."},
};
static const size_t kWsOnlyLiveCount = sizeof(kWsOnlyLive) / sizeof(kWsOnlyLive[0]);

// CLIQUET (g) : borne de l'état constaté le 2026-09-07, feature-092. Le compte
// ne doit JAMAIS augmenter — une clé neuve de la trame va dans /data. Abaissé
// 18 → 16 le 2026-10-05 (feature-140) : `filtration_state_source` et
// `filtration_ext_on` ont migré vers kDataOnly (ADR-0086, financement du
// budget WS) — la dette inventoriée se réduit, le plafond la suit. Abaissé
// 16 → 14 le 2026-10-09 (feature-146) : `filtration_force_on`/`filtration_force_off`
// ont disparu du firmware avec l'ordre direct — il n'y a plus d'écho WS à
// déclarer, l'exception est retirée plutôt que neutralisée.
static const size_t kWsOnlyLiveCeiling = 14;

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
  // ph_container_ml / orp_container_ml : feature-143 (v2.59.0) a servi ces deux
  // clefs par /data elle-meme (web_routes_data.cpp) — c'etait la 4e recidive du
  // motif de parite, corrigee en PREMIER dans ce lot. Elles ne sont plus une
  // exception dual-sourcee : /data et la trame WS les portent desormais TOUTES
  // LES DEUX, et la paire est captee par la verification generique (a)-(d).
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
  {"ph_integral",
   "Integrale pH accumulee (pH.s), ELAGUEE de la trame WS par feature-138 (option a, decision utilisateur du 2026-10-04) : plus aucun consommateur cote client depuis feature-092. Reste un contrat externe sur /data, MQTT et Home Assistant."},
  {"ph_integral_term",
   "Contribution au debit moyen demande (mL/min) du terme integral pH, ELAGUEE de la trame WS par feature-138 pour le meme motif et la meme decision que ph_integral ci-dessus — budget d'octets de sWsFrameBuf."},
  {"filtration_state_source",
   "Diagnostic d'origine du verdict de filtration (« commanded »/« powered »/« external »), ELAGUE de la trame WS par feature-140 (ADR-0086, aucun consommateur dans app.js) pour financer rescue_until/rescue_dosing — reste un contrat externe sur /data."},
  {"filtration_ext_on",
   "Verdict de filtration externe (marche/arret), ELAGUE de la trame WS par feature-140 pour le meme motif et la meme decision que filtration_state_source ci-dessus — budget d'octets de sWsFrameBuf."},
};
static const size_t kDataOnlyCount = sizeof(kDataOnly) / sizeof(kDataOnly[0]);

// ---------------------------------------------------------------------------
// CATÉGORIE 4 — SECOND AXE, IMBRIQUÉ : champs de l'objet `flow_assist` servis
// par /data SEUL. Receveur `faObj[` des DEUX côtés (nom repris à l'identique
// par le firmware, exprès : un seul extracteur textuel lit les deux fichiers).
//
// PÉRIMÈTRE ARRÊTÉ PAR bug-082 : la trame pousse `aborted` et `flow255`, les
// sept champs de SESSION ci-dessous restent /data-seuls parce qu'ils ne
// pilotent l'écran de l'assistant que PENDANT une session, et que le client
// force une charge /data à son ouverture. Le budget de trame est DUR
// (kWsFrameBufBytes = 3584 o) : un dépassement SAUTE le push entier.
//
// ⚠️ INTERDICTION EXPLICITE — `aborted` et `flow255` NE DOIVENT JAMAIS ÊTRE
// INSCRITS ICI. Leur disparition d'un côté DOIT faire échouer ce test : une
// exception qui les couvrirait reproduirait, en plus étroit, le défaut même que
// bug-082 corrige (un garde-fou de dosage silencieusement mort). Le cas (n)
// interdit ces deux clés dans la table et exige leur présence des deux côtés.
// ---------------------------------------------------------------------------
static const WsOnlyKey kFlowAssistDataOnly[] = {
  {"active",
   "Drapeau de session de l'assistant : ne pilote l'ecran que PENDANT une session, et le client force une charge /data a l'ouverture de l'assistant — la trame n'a donc rien a repousser toutes les 5 s."},
  {"pump",
   "Numero de pompe de la session (1|2, 0 = aucune) : lu a l'ouverture de l'ecran d'assistant, apres le rafraichissement /data force ; aucune decision de dosage ne le relit ensuite."},
  {"phase",
   "Phase de la marche (« purge » / « measure » / vide a l'arret) : CHAINE de caractere, la pousser a chaque trame couterait de la place dans un budget DUR de 3584 o pour un ecran deja rafraichi par /data."},
  {"remaining_s",
   "Compte a rebours de la phase en cours : le client le DECOMPTE lui-meme entre deux charges /data (resynchronisation aux echeances, feature-075), il n'a pas besoin d'un push a 5 s."},
  {"purges_left",
   "Quota de purges restant dans la session : consomme a l'affichage de l'ecran d'assistant, apres la charge /data forcee a son ouverture."},
  {"purge_s",
   "Duree de purge SERVIE (constante d'usine kFlowAssistPurgeDurationS) : invariante pendant tout le fonctionnement, une seule lecture /data suffit a parametrer les libelles."},
  {"measure_s",
   "Duree de mesure SERVIE (constante d'usine kFlowAssistMeasureDurationS) : invariante elle aussi, meme motif que purge_s — rien a repousser en continu."},
};
static const size_t kFlowAssistDataOnlyCount =
    sizeof(kFlowAssistDataOnly) / sizeof(kFlowAssistDataOnly[0]);

// CLIQUET IMBRIQUÉ (m), même mécanique que kWsOnlyLiveCeiling : borne de l'état
// arrêté le 2026-09-07 (bug-082). Un HUITIÈME champ de session ne peut pas
// s'y ajouter en silence — franchir ce plafond fait ÉCHOUER le test. La réponse
// n'est jamais de relever le plafond, c'est de servir le champ des deux côtés.
static const size_t kFlowAssistDataOnlyCeiling = 7;

// Receveur imbriqué : MÊME identifiant dans les deux fichiers du firmware.
static const char* kNestedRecv = "faObj";

// Les deux champs BILATÉRAUX, non exceptables (cf. cas (n)).
static const char* kFlowAssistBilateral[] = {"aborted", "flow255"};
static const size_t kFlowAssistBilateralCount =
    sizeof(kFlowAssistBilateral) / sizeof(kFlowAssistBilateral[0]);

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

// ===========================================================================
// SECOND AXE — extraction des champs IMBRIQUÉS (receveur `faObj[`).
// Le bloc `flow_assist` est HORS des marqueurs PARITE-DATA des deux côtés : on
// travaille donc sur la PORTÉE de la fonction, pas sur le bloc marqué.
// ===========================================================================
static void failOnNestedExtractError(const char* rel, int rc) {
  char msg[768];
  const char* what =
    rc == -1 ? "marqueur de DEBUT « >>> PARITE-DATA:DEBUT » ABSENT" :
    rc == -2 ? "marqueur de FIN « <<< PARITE-DATA:FIN » ABSENT" :
    rc == -3 ? "fichier source INTROUVABLE" :
    rc == -4 ? "marqueurs INVERSES (FIN avant DEBUT)" :
    rc == -5 ? "ancre de PORTEE (signature de fonction) INTROUVABLE" :
    rc == -6 ? "marqueur situe HORS de la portee surveillee" : "erreur inconnue";
  snprintf(msg, sizeof(msg),
           "SECOND AXE AVEUGLE dans %s : %s. Sans portee lisible, AUCUN champ "
           "imbrique de `flow_assist` n'est compare : la disparition de "
           "`aborted` de la trame — garde-fou « mesure de debit avortee », donc "
           "de DOSAGE — redeviendrait muette. QUE FAIRE : restaurer l'ancre, ou "
           "corriger celle de ce test si la fonction a ete renommee — NE PAS "
           "DESACTIVER CE TEST.", rel, what);
  TEST_FAIL_MESSAGE(msg);
}

// Charge les champs du receveur `faObj[` des DEUX côtés, avec le MÊME
// extracteur. Renvoie false si un échec a déjà été signalé.
static bool loadNestedSides(KeySet& httpFa, KeySet& wsFa) {
  KeySet ignoredMarked;
  const int rcHttp = extractScopeKeys(kHttpFile, kNestedRecv, kHttpScopeBegin,
                                      kHttpScopeEnd, httpFa, ignoredMarked);
  if (rcHttp < 0) { failOnNestedExtractError(kHttpFile, rcHttp); return false; }
  const int rcWs = extractScopeKeys(kWsFile, kNestedRecv, kWsScopeBegin,
                                    kWsScopeEnd, wsFa, ignoredMarked);
  if (rcWs < 0) { failOnNestedExtractError(kWsFile, rcWs); return false; }
  return true;
}

static bool declaredFlowAssistDataOnly(const char* k) {
  for (size_t i = 0; i < kFlowAssistDataOnlyCount; ++i) {
    if (strcmp(kFlowAssistDataOnly[i].key, k) == 0) return true;
  }
  return false;
}

// Copie du TEXTE de la portée surveillée (à libérer par l'appelant), NULL si
// une ancre manque — utilisé par les gardes de FORME (cas (o)), qui portent sur
// la façon d'écrire la valeur et non sur le seul nom de la clé.
static char* readScopeText(const char* rel, const char* scopeBegin,
                           const char* scopeEnd) {
  long len = 0;
  char* src = readProjectFile(rel, &len);
  if (src == NULL) return NULL;
  const char* b = strstr(src, scopeBegin);
  const char* e = (b != NULL) ? strstr(b, scopeEnd) : NULL;
  if (b == NULL || e == NULL) { free(src); return NULL; }
  const size_t n = (size_t)(e - b);
  char* out = (char*)malloc(n + 1);
  if (out == NULL) { free(src); return NULL; }
  memcpy(out, b, n);
  out[n] = '\0';
  free(src);
  return out;
}

static size_t countOccurrences(const char* hay, const char* needle) {
  size_t n = 0;
  const size_t nl = strlen(needle);
  for (const char* p = strstr(hay, needle); p != NULL; p = strstr(p + nl, needle)) n++;
  return n;
}

// Premier nombre décimal qui suit `needle` dans `hay` ; NAN si introuvable.
static double numberAfter(const char* hay, const char* needle) {
  const char* p = strstr(hay, needle);
  if (p == NULL) return -1.0;
  p += strlen(needle);
  while (*p != '\0' && (*p < '0' || *p > '9') && *p != '-' && *p != '.') {
    if (*p == '\n') return -1.0;  // ne franchit pas la ligne
    ++p;
  }
  char* end = NULL;
  const double v = strtod(p, &end);
  if (end == p) return -1.0;
  return v;
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
// (h) EXCLUSION DES RECEVEURS IMBRIQUÉS DU DIFF DE PREMIER NIVEAU : les champs
// de `flow_assist` sont écrits via `faObj[` et ne doivent JAMAIS être comptés
// comme des clés de premier niveau — sinon le diff se remplirait de fausses
// clés « /data-seules » et la sentinelle deviendrait illisible.
//
// La liste n'est plus écrite en dur : elle avait VIEILLI dès que le firmware a
// servi `flow_assist` des deux côtés (bug-082), et une liste périmée ici
// n'échoue pas, elle s'affadit. Elle est donc DÉRIVÉE du second axe : tout
// champ imbriqué constaté, d'un côté ou de l'autre, est vérifié absent des deux
// inventaires de premier niveau. Ce qui reste vrai, et le restera : un champ
// interne n'est pas une clé de premier niveau.
// ---------------------------------------------------------------------------
void test_h_les_receveurs_imbriques_sont_exclus_du_diff(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;
  KeySet httpFa, wsFa;
  if (!loadNestedSides(httpFa, wsFa)) return;

  KeySet nested;
  nested.count = 0;
  for (size_t i = 0; i < httpFa.count; ++i) keySetAdd(nested, httpFa.keys[i]);
  for (size_t i = 0; i < wsFa.count; ++i)   keySetAdd(nested, wsFa.keys[i]);

  char msg[768];
  // Plancher DÉCOUPLÉ de la taille de la table, comme (j) : on exige seulement
  // que la liste dérivée ne soit pas vide de sens (les deux champs bilatéraux).
  snprintf(msg, sizeof(msg),
           "GARDE (h) VIDE : %u champ(s) imbrique(s) derive(s) du second axe, "
           "pour %u au minimum. Une liste vide ferait PASSER ce test sans rien "
           "verifier. QUE FAIRE : verifier l'extraction du receveur `%s[` "
           "(cas (j)) avant de toucher a celui-ci. NE PAS DESACTIVER CE TEST.",
           (unsigned)nested.count, (unsigned)kFlowAssistBilateralCount,
           kNestedRecv);
  TEST_ASSERT_TRUE_MESSAGE(nested.count >= kFlowAssistBilateralCount, msg);

  for (size_t i = 0; i < nested.count; ++i) {
    snprintf(msg, sizeof(msg),
             "RECEVEUR IMBRIQUE CAPTE : « %s » est un champ de l'objet "
             "`flow_assist` (ecrit via `%s[`), il a ete compte comme une cle de "
             "PREMIER NIVEAU. Le diff se remplirait de fausses cles "
             "/data-seules et la sentinelle deviendrait illisible. QUE FAIRE : "
             "restaurer la contrainte d'identifiant COMPLET sur le receveur "
             "dans collectKeys(). NE PAS DESACTIVER CE TEST.",
             nested.keys[i], kNestedRecv);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(http, nested.keys[i]), msg);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(ws, nested.keys[i]), msg);
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

// ===========================================================================
// SECOND AXE — CONTENU DE `flow_assist` (cas (j) à (n))
// ===========================================================================

// ---------------------------------------------------------------------------
// (j) Le receveur imbriqué existe des DEUX côtés, sous le MÊME nom, et livre
// assez de champs pour que le diff ait un sens. Un extracteur qui ne trouve
// rien ferait PASSER tout le reste de l'axe.
// ---------------------------------------------------------------------------
void test_j_le_receveur_imbrique_existe_des_deux_cotes(void) {
  KeySet httpFa, wsFa;
  if (!loadNestedSides(httpFa, wsFa)) return;

  char msg[900];

  // PLANCHER DÉCOUPLÉ DE LA TAILLE DE LA TABLE (revue) : chaque côté est jugé
  // sur SON propre compte, contre le même plancher fixe — les deux champs
  // bilatéraux. Un plancher indexé sur kFlowAssistDataOnlyCount faisait accuser
  // handleGetData quand on grossissait la table pour couvrir un manque situé
  // dans la trame : le message envoyait corriger le MAUVAIS fichier. Le reste
  // de l'inventaire est déjà jugé par (k), (l) et (m).
  snprintf(msg, sizeof(msg),
           "RECEVEUR IMBRIQUE DISPARU (ou RENOMME) dans %s : %u champ(s) "
           "`%s[\"...\"]` extrait(s) dans handleGetData, pour %u au minimum "
           "(`aborted` et `flow255`, bilateraux par contrat). Avec zero champ "
           "extrait, TOUT le second axe passerait sans rien comparer. QUE "
           "FAIRE : restaurer le bloc `flow_assist` DANS CE FICHIER ; s'il a "
           "ete RENOMME, le renommer A L'IDENTIQUE DES DEUX COTES — c'est ce "
           "nom partage qui permet un seul extracteur — puis mettre kNestedRecv "
           "a jour. NE PAS DESACTIVER CE TEST.",
           kHttpFile, (unsigned)httpFa.count, kNestedRecv,
           (unsigned)kFlowAssistBilateralCount);
  TEST_ASSERT_TRUE_MESSAGE(httpFa.count >= kFlowAssistBilateralCount, msg);

  snprintf(msg, sizeof(msg),
           "RECEVEUR IMBRIQUE DISPARU (ou RENOMME) dans %s : %u champ(s) "
           "`%s[\"...\"]` extrait(s) dans _buildSensorJson, pour %u attendus "
           "(`aborted` et `flow255`). C'est le mode d'echec exact de bug-082 : "
           "sans bloc `flow_assist` dans la trame, `aborted` redevient MORT en "
           "pratique (la trame ecrase latestSensorData avant la saisie du "
           "volume) et une mesure de debit AVORTEE repart vers le calcul de "
           "dosage — pouls allonge ET dailyInjectedMl sous-compte. QUE FAIRE : "
           "restaurer le bloc DANS CE FICHIER, pas cette assertion. NE PAS "
           "DESACTIVER CE TEST.",
           kWsFile, (unsigned)wsFa.count, kNestedRecv,
           (unsigned)kFlowAssistBilateralCount);
  TEST_ASSERT_TRUE_MESSAGE(wsFa.count >= kFlowAssistBilateralCount, msg);
}

// ---------------------------------------------------------------------------
// (k) Parité du CONTENU, dans les deux sens. Sens trame -> /data : AUCUNE
// exception admise, volontairement (il n'existe pas de table « WS-seul »
// imbriquée). Sens /data -> trame : déclaration obligatoire.
// ---------------------------------------------------------------------------
void test_k_aucun_champ_imbrique_non_declare(void) {
  KeySet httpFa, wsFa;
  if (!loadNestedSides(httpFa, wsFa)) return;

  for (size_t i = 0; i < wsFa.count; ++i) {
    const char* k = wsFa.keys[i];
    char msg[900];
    snprintf(msg, sizeof(msg),
             "PARITE IMBRIQUEE ROMPUE (sens trame) : « flow_assist.%s » est "
             "pousse par la trame `sensor_data` (%s) mais ABSENT du meme objet "
             "dans GET /data (%s). Il ne manquera pas : il CLIGNOTERA, comme "
             "toute clé servie d'un seul cote. QUE FAIRE : servir le champ dans "
             "le bloc `%s` de handleGetData, avec la MEME orthographe. Il "
             "n'existe VOLONTAIREMENT aucune table d'exception pour ce sens : "
             "un champ que la trame juge assez important pour etre pousse "
             "toutes les 5 s doit survivre a une charge /data. NE PAS "
             "DESACTIVER CE TEST.", k, kWsFile, kHttpFile, kNestedRecv);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(httpFa, k), msg);
  }

  for (size_t i = 0; i < httpFa.count; ++i) {
    const char* k = httpFa.keys[i];
    if (keySetHas(wsFa, k)) continue;
    char msg[900];
    snprintf(msg, sizeof(msg),
             "PARITE IMBRIQUEE ROMPUE (sens /data) : « flow_assist.%s » est "
             "servi par GET /data (%s) mais ABSENT du bloc `%s` de la trame "
             "(%s) et NON DECLARE. QUE FAIRE — au choix : (1) AJOUTER le champ "
             "au bloc `%s` de _buildSensorJson, en verifiant le budget (plafond "
             "DUR kWsFrameBufBytes = 3584 o, un depassement SAUTE le push) ; OU "
             "(2) L'INSCRIRE dans kFlowAssistDataOnly avec un motif de >= %u "
             "caracteres disant POURQUOI le client peut s'en passer entre deux "
             "charges /data — mais le cliquet (m) interdit d'agrandir cette "
             "table. NE PAS DESACTIVER CE TEST.",
             k, kHttpFile, kNestedRecv, kWsFile, kNestedRecv,
             (unsigned)kMinReasonLen);
    TEST_ASSERT_TRUE_MESSAGE(declaredFlowAssistDataOnly(k), msg);
  }
}

// ---------------------------------------------------------------------------
// (l) Aucune exception imbriquée MORTE ni CONTREDITE — même exigence que (e),
// c'est ce mécanisme qui a fait tomber `flow_assist` de kDataOnly.
// ---------------------------------------------------------------------------
void test_l_aucune_exception_imbriquee_morte_ni_contredite(void) {
  KeySet httpFa, wsFa;
  if (!loadNestedSides(httpFa, wsFa)) return;

  char msg[900];
  for (size_t j = 0; j < kFlowAssistDataOnlyCount; ++j) {
    const char* k = kFlowAssistDataOnly[j].key;
    snprintf(msg, sizeof(msg),
             "EXCEPTION IMBRIQUEE MORTE : « flow_assist.%s » est declaree "
             "/data-seule dans kFlowAssistDataOnly, mais ce champ n'est PLUS "
             "servi par GET /data (%s). Une exception qui ne protege plus rien "
             "transforme la table en decharge de justifications perimees et "
             "masquera la prochaine vraie divergence. QUE FAIRE : SUPPRIMER "
             "l'entree (et baisser kFlowAssistDataOnlyCeiling d'autant), ou "
             "restaurer le champ si son retrait est un accident. NE PAS "
             "DESACTIVER CE TEST.", k, kHttpFile);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(httpFa, k), msg);

    snprintf(msg, sizeof(msg),
             "EXCEPTION IMBRIQUEE CONTREDITE : « flow_assist.%s » est declaree "
             "/data-seule alors qu'elle EST desormais poussee par la trame "
             "`sensor_data` (%s). Le motif ecrit ne decrit plus le code. QUE "
             "FAIRE : SUPPRIMER l'entree de kFlowAssistDataOnly (la parite est "
             "atteinte pour ce champ) et baisser le cliquet. NE PAS DESACTIVER "
             "CE TEST.", k, kWsFile);
    TEST_ASSERT_FALSE_MESSAGE(keySetHas(wsFa, k), msg);
  }
}

// ---------------------------------------------------------------------------
// (m) Motif écrit, aucun doublon, CLIQUET, inventaire cohérent.
// ---------------------------------------------------------------------------
void test_m_cliquet_et_motifs_de_la_table_imbriquee(void) {
  KeySet httpFa, wsFa;
  if (!loadNestedSides(httpFa, wsFa)) return;

  char msg[900];
  for (size_t j = 0; j < kFlowAssistDataOnlyCount; ++j) {
    const char* k = kFlowAssistDataOnly[j].key;
    const char* r = kFlowAssistDataOnly[j].reason;
    snprintf(msg, sizeof(msg),
             "EXCEPTION SANS MOTIF : « flow_assist.%s » (kFlowAssistDataOnly) "
             "n'a pas de justification ecrite d'au moins %u caracteres. QUE "
             "FAIRE : ecrire POURQUOI le client peut se passer de ce champ "
             "entre deux charges /data (rafraichissement force, valeur "
             "invariante, decompte local...) — un « /data-seul » sans motif est "
             "indiscernable d'un oubli. NE PAS DESACTIVER CE TEST.",
             k, (unsigned)kMinReasonLen);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, msg);
    TEST_ASSERT_TRUE_MESSAGE(strlen(r) >= kMinReasonLen, msg);
  }

  for (size_t a = 0; a < kFlowAssistDataOnlyCount; ++a) {
    for (size_t b = a + 1; b < kFlowAssistDataOnlyCount; ++b) {
      snprintf(msg, sizeof(msg),
               "EXCEPTION IMBRIQUEE EN DOUBLE : « %s » apparait deux fois dans "
               "kFlowAssistDataOnly. QUE FAIRE : fusionner en une seule entree, "
               "avec le motif le plus complet.", kFlowAssistDataOnly[a].key);
      TEST_ASSERT_FALSE_MESSAGE(strcmp(kFlowAssistDataOnly[a].key,
                                       kFlowAssistDataOnly[b].key) == 0, msg);
    }
  }

  snprintf(msg, sizeof(msg),
           "CLIQUET IMBRIQUE FRANCHI : %u entree(s) dans kFlowAssistDataOnly "
           "pour un plafond de %u. Le perimetre /data-seul de `flow_assist` a "
           "ete ARRETE a sept champs de session (bug-082) : un HUITIEME ne s'y "
           "ajoute pas en silence. QUE FAIRE : servir le champ des deux cotes "
           "(bloc `%s` de _buildSensorJson). NE PAS relever le plafond.",
           (unsigned)kFlowAssistDataOnlyCount,
           (unsigned)kFlowAssistDataOnlyCeiling, kNestedRecv);
  TEST_ASSERT_TRUE_MESSAGE(kFlowAssistDataOnlyCount <= kFlowAssistDataOnlyCeiling,
                           msg);

  size_t dataOnly = 0;
  for (size_t i = 0; i < httpFa.count; ++i) {
    if (!keySetHas(wsFa, httpFa.keys[i])) dataOnly++;
  }
  snprintf(msg, sizeof(msg),
           "INVENTAIRE IMBRIQUE INCOHERENT : %u champ(s) /data-seul(s) "
           "constate(s) dans `flow_assist` pour %u entree(s) declarees. QUE "
           "FAIRE : servir le champ manquant des deux cotes, OU le declarer ; "
           "si une exception est devenue inutile, la supprimer. NE PAS "
           "DESACTIVER CE TEST.",
           (unsigned)dataOnly, (unsigned)kFlowAssistDataOnlyCount);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(kFlowAssistDataOnlyCount, dataOnly, msg);
}

// ---------------------------------------------------------------------------
// (n) LES DEUX CHAMPS BILATÉRAUX NE SONT PAS EXCEPTABLES. C'est l'objet même du
// second axe : `aborted` et `flow255` doivent être servis des deux côtés, et
// AUCUNE table ne doit pouvoir couvrir leur disparition — sinon on aurait
// reproduit, en plus étroit, le défaut que bug-082 corrige.
// ---------------------------------------------------------------------------
void test_n_les_champs_bilateraux_de_flow_assist_ne_sont_pas_exceptables(void) {
  KeySet http, httpMarked, ws, wsMarked;
  if (!loadBothSides(http, httpMarked, ws, wsMarked)) return;
  KeySet httpFa, wsFa;
  if (!loadNestedSides(httpFa, wsFa)) return;

  char msg[900];

  // Le receveur lui-même est une clé de premier niveau BILATÉRALE depuis
  // bug-082 : c'est la condition d'existence de tout le second axe.
  snprintf(msg, sizeof(msg),
           "OBJET `flow_assist` NON BILATERAL : il doit etre servi par GET "
           "/data (%s) ET par la trame `sensor_data` (%s). S'il repasse "
           "/data-seul, ses champs redeviennent `undefined` en ~5 s (la trame "
           "REMPLACE latestSensorData en entier) — c'est bug-082, 5e recidive "
           "du motif de parite. NE PAS re-declarer la cle dans kDataOnly : "
           "servir l'objet des deux cotes. NE PAS DESACTIVER CE TEST.",
           kHttpFile, kWsFile);
  TEST_ASSERT_TRUE_MESSAGE(keySetHas(http, "flow_assist"), msg);
  TEST_ASSERT_TRUE_MESSAGE(keySetHas(ws, "flow_assist"), msg);

  for (size_t i = 0; i < kFlowAssistBilateralCount; ++i) {
    const char* k = kFlowAssistBilateral[i];

    snprintf(msg, sizeof(msg),
             "CHAMP BILATERAL PERDU : « flow_assist.%s » doit etre servi par "
             "GET /data ET par la trame `sensor_data`. Pour `aborted`, c'est un "
             "GARDE-FOU DE DOSAGE : il signale une mesure de debit interrompue "
             "par le serveur (perte de filtration), et son absence laisse "
             "router une mesure AVORTEE vers la saisie du volume — debit "
             "sous-estime, donc pouls ALLONGE (sur-injection reelle) ET "
             "dailyInjectedMl sous-compte (garde maxDailyMl desserree du meme "
             "facteur). Pour `flow255`, l'UI retombe sur son repli 61/67 et "
             "annonce un cout budget faux. NE PAS DESACTIVER CE TEST.", k);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(httpFa, k), msg);
    TEST_ASSERT_TRUE_MESSAGE(keySetHas(wsFa, k), msg);

    snprintf(msg, sizeof(msg),
             "EXCEPTION INTERDITE : « %s » a ete inscrit dans "
             "kFlowAssistDataOnly. Ces deux champs (`aborted`, `flow255`) sont "
             "BILATERAUX par contrat : les declarer /data-seuls rendrait leur "
             "disparition de la trame ACCEPTABLE pour la sentinelle, c'est-a-"
             "dire reproduirait bug-082 en plus etroit — un garde-fou de dosage "
             "mort sans qu'aucun test ne bronche. QUE FAIRE : retirer l'entree "
             "et servir le champ des deux cotes. NE PAS DESACTIVER CE TEST.", k);
    TEST_ASSERT_FALSE_MESSAGE(declaredFlowAssistDataOnly(k), msg);
  }
}

// ===========================================================================
// GARDES DE COMPORTEMENT demandées par le visa chimique (cas (o) et (p))
// ===========================================================================

// ---------------------------------------------------------------------------
// (o) FORME DE `flow255` : TABLEAU DE DEUX ÉLÉMENTS, des deux côtés, jamais un
// scalaire. Ce n'est pas de la cosmétique de contrat : côté client, un scalaire
// n'est pas une erreur bruyante, il fait retomber flow255For() sur son repli —
// donc l'UI annonce un coût budget calculé sur un débit qui n'est pas celui de
// la pompe. Le nom de la clé (couvert par (k)) ne dit rien de sa FORME : c'est
// cette assertion qui la verrouille.
// ---------------------------------------------------------------------------
void test_o_flow255_est_un_tableau_de_deux_elements_des_deux_cotes(void) {
  const char* rels[2]  = {kHttpFile, kWsFile};
  const char* begs[2]  = {kHttpScopeBegin, kWsScopeBegin};
  const char* ends[2]  = {kHttpScopeEnd, kWsScopeEnd};

  for (int i = 0; i < 2; ++i) {
    char* scope = readScopeText(rels[i], begs[i], ends[i]);
    char msg[900];
    snprintf(msg, sizeof(msg),
             "PORTEE ILLISIBLE dans %s : ancre de fonction introuvable, la "
             "garde de FORME de `flow255` ne verifie rien. QUE FAIRE : "
             "corriger l'ancre dans ce test. NE PAS DESACTIVER CE TEST.",
             rels[i]);
    TEST_ASSERT_NOT_NULL_MESSAGE(scope, msg);
    if (scope == NULL) return;

    snprintf(msg, sizeof(msg),
             "FORME DE `flow255` PERDUE dans %s : le champ n'est plus construit "
             "comme un TABLEAU (`%s[\"flow255\"].to<JsonArray>()` introuvable "
             "dans la portee). Un scalaire ne leve AUCUNE erreur cote client : "
             "flow255For() ne sait pas a quelle pompe il appartient, l'ignore, "
             "et retombe sur son repli 61/67 — l'UI annonce alors un cout "
             "budget calcule sur un debit qui n'est pas celui de la pompe. QUE "
             "FAIRE : restaurer le tableau des deux cotes. NE PAS DESACTIVER "
             "CE TEST.", rels[i], kNestedRecv);
    char arrayNeedle[64];
    snprintf(arrayNeedle, sizeof(arrayNeedle), "%s[\"flow255\"].to<JsonArray>()",
             kNestedRecv);
    TEST_ASSERT_TRUE_MESSAGE(strstr(scope, arrayNeedle) != NULL, msg);

    char scalarNeedle[64];
    snprintf(scalarNeedle, sizeof(scalarNeedle), "%s[\"flow255\"] =", kNestedRecv);
    snprintf(msg, sizeof(msg),
             "`flow255` SERVI EN SCALAIRE dans %s : affectation directe "
             "detectee. Le client REFUSE un scalaire (Array.isArray) et "
             "retombe sur son repli : la valeur servie serait perdue en "
             "silence. QUE FAIRE : servir un tableau de DEUX elements "
             "([0] = pompe 1, [1] = pompe 2). NE PAS DESACTIVER CE TEST.",
             rels[i]);
    TEST_ASSERT_TRUE_MESSAGE(strstr(scope, scalarNeedle) == NULL, msg);

    const size_t adds = countOccurrences(scope, ".add(kPumpFlow255MlPerMin)");
    snprintf(msg, sizeof(msg),
             "CARDINAL DE `flow255` FAUX dans %s : %u ajout(s) "
             "`.add(kPumpFlow255MlPerMin)` dans la portee, 2 attendus ([0] = "
             "pompe 1, [1] = pompe 2). Un tableau d'UN element rend "
             "`flow255[1]` indefini : la pompe 2 retombe silencieusement sur le "
             "repli 67 mL/min. QUE FAIRE : servir les DEUX elements. NE PAS "
             "DESACTIVER CE TEST.", rels[i], (unsigned)adds);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, adds, msg);

    free(scope);
  }
}

// ---------------------------------------------------------------------------
// (p) LE REPLI CLIENT EST LE DÉTECTEUR : `FLOW_255_FALLBACK` (61 / 67, débits
// des firmwares antérieurs à la fusion de flotte) doit rester DISTINCT de
// kPumpFlow255MlPerMin (74). L'aligner sur 74 rendrait la présence du champ
// INDISCERNABLE de son absence — bug-082 serait devenu invisible pour toujours,
// et la 6ᵉ récidive du motif de parité, muette. Cette assertion croise les deux
// fichiers : elle échoue si l'un des deux nombres bouge vers l'autre.
// ---------------------------------------------------------------------------
void test_p_le_repli_client_flow255_reste_distinct_de_la_constante_firmware(void) {
  long len = 0;
  char* app = readProjectFile("data/app.js", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(app,
      "data/app.js INTROUVABLE : le repli client de `flow255` ne peut pas etre "
      "compare a la constante firmware. NE PAS DESACTIVER CE TEST.");
  if (app == NULL) return;

  char* consts = readProjectFile("src/constants.h", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(consts, "src/constants.h INTROUVABLE.");
  if (consts == NULL) { free(app); return; }

  const double k = numberAfter(consts, "kPumpFlow255MlPerMin = ");
  const char* fb = strstr(app, "FLOW_255_FALLBACK");
  char msg[900];
  snprintf(msg, sizeof(msg),
           "REPLI CLIENT INTROUVABLE : `FLOW_255_FALLBACK` a disparu de "
           "data/app.js, ou kPumpFlow255MlPerMin de src/constants.h (lu : %g). "
           "Sans les deux valeurs, on ne peut plus prouver qu'elles sont "
           "DISTINCTES — et c'est cette distinction qui rend un `flow255` "
           "manquant detectable. NE PAS DESACTIVER CE TEST.", k);
  TEST_ASSERT_NOT_NULL_MESSAGE(fb, msg);
  TEST_ASSERT_TRUE_MESSAGE(k > 0.0, msg);
  if (fb == NULL || !(k > 0.0)) { free(app); free(consts); return; }

  const double f1 = numberAfter(fb, "1:");
  const double f2 = numberAfter(fb, "2:");
  for (int i = 0; i < 2; ++i) {
    const double f = (i == 0) ? f1 : f2;
    snprintf(msg, sizeof(msg),
             "REPLI ALIGNE SUR LA CONSTANTE FIRMWARE : "
             "FLOW_255_FALLBACK[%d] = %g et kPumpFlow255MlPerMin = %g. Le repli "
             "vaut les debits des firmwares ANTERIEURS a la fusion de flotte "
             "(61 pompe 1, 67 pompe 2), qui ne publient pas `flow255` et dosent "
             "reellement a ces debits. L'aligner sur %g fait DEUX degats : (1) "
             "sur firmware ancien, l'estimation passe 18 a 21 %% sous la marche "
             "reelle, l'utilisateur croit a un defaut et RELANCE — la "
             "sur-injection vient de cette repetition ; (2) sur firmware a "
             "jour, la presence du champ devient INDISCERNABLE de son absence, "
             "donc bug-082 invisible pour toujours. LE REPLI DISTINCT EST LE "
             "DETECTEUR : ne pas l'aligner. NE PAS DESACTIVER CE TEST.",
             i + 1, f, k, k);
    TEST_ASSERT_TRUE_MESSAGE(f > 0.0, msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(f - k) > 0.5, msg);
  }

  free(app);
  free(consts);
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// (q) PARITÉ D'EXPRESSION — l'angle mort que les axes (a)-(p) laissaient ouvert.
//
// DÉCOUVERT PAR MUTATION (feature-152, 2026-10-11). Tous les axes ci-dessus
// comparent des JEUX DE CLÉS. Ils sont donc AVEUGLES au cas où une clé est
// présente des deux côtés mais CALCULÉE DIFFÉREMMENT. Preuve : remplacer, dans
// ws_manager.cpp SEULEMENT,
//     d["measures_valid"] = mv.measuresValid;
// par
//     d["measures_valid"] = mv.measuresValid && !isnan(phFiltered);
// laissait la suite ENTIÈREMENT VERTE (16/16).
//
// POURQUOI C'EST LE MÊME DÉFAUT, EN PIRE. `latestSensorData` est REMPLACÉ en
// entier par la dernière charge reçue (data/app.js, aucune fusion). Deux
// expressions divergentes pour une même clé font donc OSCILLER sa valeur au
// rythme des deux cadences (~5 s / ~30 s) — exactement le CLIGNOTEMENT du piège
// n° 1, sauf qu'ici la clé est présente des deux côtés : le défaut est INVISIBLE
// à la lecture d'une seule charge, et aucun des axes précédents ne le voit.
//
// PÉRIMÈTRE VOLONTAIREMENT ÉTROIT. Exiger l'égalité textuelle sur TOUTES les
// clés produirait un torrent de faux positifs légitimes (arrondis, variables
// locales, types). Cette table ne porte donc que les clés dont l'expression
// ELLE-MÊME est le contrat — celles dont une divergence silencieuse change ce
// que l'utilisateur voit. On y ajoute une clé quand on constate que sa valeur,
// et pas seulement sa présence, doit être la même des deux côtés.
//
// NORMALISATION : les deux fichiers nomment différemment les mêmes locales
// (`phFil`/`phFiltered`, `orpFil`/`orpFiltered`). Les alias ci-dessous sont
// appliqués avant comparaison, et les espaces retirés. Toute AUTRE différence
// est un échec.
// ---------------------------------------------------------------------------
struct SharedExprKey {
  const char* key;
  const char* why;
};
static const SharedExprKey kSharedExpr[] = {
  { "measures_valid",
    "Pilote measuresAreValid() cote client, qui gate la TEMPERATURE et les DEUX "
    "voies. Une divergence entre /data et le WS fait apparaitre et disparaitre "
    "la temperature au rythme des deux cadences." },
  { "cal_switch_active",
    "bug-160 (volet 2, REFONTE 2026-10-13) : pilote le bandeau de bascule "
    "pH+ORP en Manuel pendant une calibration (remplace cal_hold_remaining_s, "
    "bail ABANDONNE). Une divergence d'expression ferait apparaitre et "
    "disparaitre le bandeau de securite au rythme des deux cadences (~5 s / "
    "~30 s)." },
  { "cal_session_open",
    "feature-163 (v2.83.0) : pilote le bandeau de securite d'etalonnage depuis "
    "le predicat REEL (une session est ouverte), la ou cal_switch_active ne dit "
    "que « j'ai bascule la regulation ». Une divergence d'expression ferait "
    "apparaitre et disparaitre le bandeau au rythme des deux cadences, dans le "
    "cas meme ou le dosage est gele sans que rien ne le dise." },
  { "cal_switch_epoch",
    "bug-160 (volet 2, REFONTE 2026-10-13) : date de la bascule affichee dans "
    "le bandeau et utilisee par l'alerte d'oubli (C4). Meme risque de "
    "clignotement qu'un champ de securite si les deux cotes divergent dans "
    "leur calcul." },
  { "rescue_until",
    "bug-161 (gate pool-chemistry C4) : la carte ORP derive l'etat secours "
    "actif de rescue_until > 0 (meme predicat que l'offre L7, app.js:7471) "
    "pour decider d'ouvrir la modale de confirmation avant un passage en "
    "Arret. Une divergence d'expression ferait apparaitre et disparaitre "
    "cette modale au rythme des deux cadences (~5 s / ~30 s)." },
};
static const size_t kSharedExprCount = sizeof(kSharedExpr) / sizeof(kSharedExpr[0]);

// Alias de locales : {dans ws_manager.cpp, forme canonique}. Appliques APRES
// retrait des espaces. L'ordre compte : le plus long d'abord.
static const char* kLocalAliases[][2] = {
  { "phFiltered",  "phFil"  },
  { "orpFiltered", "orpFil" },
};
static const size_t kLocalAliasCount = sizeof(kLocalAliases) / sizeof(kLocalAliases[0]);

// Membre droit de `recv["key"] = ... ;` dans `scope`, espaces retires et alias
// normalises. Retourne NULL si l'affectation est introuvable. A liberer.
static char* extractNormalizedRhs(const char* scope, const char* recv, const char* key) {
  char needle[128];
  snprintf(needle, sizeof(needle), "%s[\"%s\"]", recv, key);
  const char* p = strstr(scope, needle);
  if (p == NULL) return NULL;
  p += strlen(needle);
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '=') return NULL;   // `.to<JsonArray>()` ou autre forme : hors perimetre
  p++;
  const char* end = strchr(p, ';');
  if (end == NULL) return NULL;

  char* out = (char*)malloc((size_t)(end - p) + 1);
  if (out == NULL) return NULL;
  size_t n = 0;
  for (const char* q = p; q < end; ++q) {
    if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') continue;
    out[n++] = *q;
  }
  out[n] = '\0';

  // Normalisation des alias de locales, en place (le remplacement RACCOURCIT
  // toujours la chaine : "phFiltered" -> "phFil").
  for (size_t a = 0; a < kLocalAliasCount; ++a) {
    const char* from = kLocalAliases[a][0];
    const char* to   = kLocalAliases[a][1];
    const size_t lf = strlen(from), lt = strlen(to);
    char* hit = strstr(out, from);
    while (hit != NULL) {
      memcpy(hit, to, lt);
      memmove(hit + lt, hit + lf, strlen(hit + lf) + 1);
      hit = strstr(hit + lt, from);
    }
  }
  return out;
}

void test_q_les_cles_a_expression_partagee_sont_calculees_a_l_identique(void) {
  char* httpScope = readScopeText(kHttpFile, kHttpScopeBegin, kHttpScopeEnd);
  TEST_ASSERT_NOT_NULL_MESSAGE(httpScope,
      "PORTEE ILLISIBLE dans src/web_routes_data.cpp : la parite d'EXPRESSION "
      "ne verifie rien. QUE FAIRE : corriger l'ancre. NE PAS DESACTIVER CE TEST.");
  char* wsScope = readScopeText(kWsFile, kWsScopeBegin, kWsScopeEnd);
  TEST_ASSERT_NOT_NULL_MESSAGE(wsScope,
      "PORTEE ILLISIBLE dans src/ws_manager.cpp : la parite d'EXPRESSION ne "
      "verifie rien. QUE FAIRE : corriger l'ancre. NE PAS DESACTIVER CE TEST.");
  if (httpScope == NULL || wsScope == NULL) { free(httpScope); free(wsScope); return; }

  for (size_t i = 0; i < kSharedExprCount; ++i) {
    const char* key = kSharedExpr[i].key;
    char msg[1400];

    char* httpRhs = extractNormalizedRhs(httpScope, "doc", key);
    snprintf(msg, sizeof(msg),
             "EXPRESSION DE `%s` INTROUVABLE dans src/web_routes_data.cpp "
             "(forme attendue : doc[\"%s\"] = ... ;). La cle est declaree a "
             "EXPRESSION PARTAGEE : si elle a change de forme, mettre ce test a "
             "jour SCIEMMENT. NE PAS DESACTIVER CE TEST.", key, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(httpRhs, msg);

    char* wsRhs = extractNormalizedRhs(wsScope, "d", key);
    snprintf(msg, sizeof(msg),
             "EXPRESSION DE `%s` INTROUVABLE dans src/ws_manager.cpp (forme "
             "attendue : d[\"%s\"] = ... ;). NE PAS DESACTIVER CE TEST.", key, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(wsRhs, msg);
    if (httpRhs == NULL || wsRhs == NULL) { free(httpRhs); free(wsRhs); continue; }

    snprintf(msg, sizeof(msg),
             "PARITE D'EXPRESSION ROMPUE sur `%s` : les deux cotes servent la "
             "cle mais ne la CALCULENT PAS pareil.\n"
             "  /data : %s\n"
             "  WS    : %s\n"
             "latestSensorData est REMPLACE en entier par la derniere charge "
             "recue : la valeur va donc OSCILLER au rythme des deux cadences "
             "(~5 s / ~30 s), sans qu'aucune charge prise isolement paraisse "
             "fausse. MOTIF DE LA DECLARATION : %s\n"
             "QUE FAIRE : aligner les deux expressions. Si la divergence est "
             "VOULUE, retirer la cle de kSharedExpr EN ECRIVANT POURQUOI. NE "
             "PAS DESACTIVER CE TEST.",
             key, httpRhs, wsRhs, kSharedExpr[i].why);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(httpRhs, wsRhs, msg);

    free(httpRhs);
    free(wsRhs);
  }

  free(httpScope);
  free(wsScope);
}

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
  // Second axe — CONTENU de `flow_assist` (receveur imbrique `faObj[`).
  RUN_TEST(test_j_le_receveur_imbrique_existe_des_deux_cotes);
  RUN_TEST(test_k_aucun_champ_imbrique_non_declare);
  RUN_TEST(test_l_aucune_exception_imbriquee_morte_ni_contredite);
  RUN_TEST(test_m_cliquet_et_motifs_de_la_table_imbriquee);
  RUN_TEST(test_n_les_champs_bilateraux_de_flow_assist_ne_sont_pas_exceptables);
  // Gardes de forme et de repli (visa chimique).
  RUN_TEST(test_o_flow255_est_un_tableau_de_deux_elements_des_deux_cotes);
  RUN_TEST(test_p_le_repli_client_flow255_reste_distinct_de_la_constante_firmware);

  RUN_TEST(test_q_les_cles_a_expression_partagee_sont_calculees_a_l_identique);
  return UNITY_END();
}
