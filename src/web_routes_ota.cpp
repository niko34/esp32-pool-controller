#include "web_routes_ota.h"
#include "web_helpers.h"
#include "constants.h"
#include "auth.h"
#include "logger.h"
#include "version.h"
#include "pump_controller.h"
#include "github_root_ca.h"
#include "ota_integrity.h"
#include "ota_integrity_logic.h"
#include "ota_check_logic.h"
// bug-echec-ota-indechiffrable-et-noyade-du-journal (2.20.1) : pré-contrôle pur
// du couple « type demandé ↔ octet magique reçu » + détection du type par nom
// de fichier (le nom nu `littlefs.bin` produit par build_fs.sh n'était reconnu
// par aucun des suffixes historiques).
#include "ota_type_logic.h"
// bug-oom-accept-http-durcissement / bug-garde-tls-trop-permissive-sans-psram :
// prédicat pur oomTlsGateReason() partagé avec le parachute OOM (garde PSRAM
// LIBRE + heap avant toute session TLS sortante : /check-update, /download-update).
#include "oom_guard_logic.h"
// bug-165/bug-108 (v2.81.0) : la garde TLS décide désormais sur la métrique 8
// bits RÉELLEMENT allouable (ADR-0058), jamais sur ESP.getFreeHeap()/
// getMaxAllocHeap() (IRAM-inclusives, aveugles à la famine 8 bits visée) — et
// sur le PLUS GROS BLOC PSRAM (contiguïté), pas la PSRAM libre totale.
#include "heap_metrics.h"
#include <esp_heap_caps.h>  // heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
// bug-173 : lecture bornée de la réponse /check-update dans un tampon PSRAM
// (logique PURE, testable en natif — ADR-0017) + témoin anti-décoratif de
// placement PSRAM, même idiome que mbedtls_psram_alloc.cpp.
#include "ota_response_buffer_logic.h"
#include <esp_memory_utils.h>  // esp_ptr_external_ram
// feature-151 D1 : vérification d'AUTHENTICITÉ (signature à clé publique) —
// JOURNALISE SEULEMENT, ne refuse rien (le refus est D3, hors périmètre de ce
// lot). Modules purs testables en natif (grammaire manifeste, base64,
// plancher, combinaison du verdict) + wrapper mbedtls non testable en natif.
#include "ota_manifest_logic.h"
#include "ota_base64_logic.h"
#include "ota_authenticity_logic.h"
#include "ota_signature.h"
// feature-151 D1, lot B1 (revue de sécurité du 2026-10-18) : la signature
// n'autorisait AUCUNE image en particulier — ce module PUR ferme ce trou en
// liant l'empreinte DÉCLARÉE par le manifeste à une empreinte connue
// indépendamment (paramètre déjà fourni, ou empreinte réellement calculée).
// JOURNALISE SEULEMENT, comme le reste de D1.
#include "ota_manifest_binding_logic.h"
// feature-063 : include EXPLICITE de WiFiClient.h — il n'arrive plus
// transitivement via HTTPClient.h depuis le core Arduino 3 (où WiFiClient est
// un alias de NetworkClient).
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>  // strlen (feature-151 D1, computeOtaAuthenticityVerdict)
#include <esp_partition.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

// Variables pour gérer les redémarrages différés (partagées avec web_server)
static bool* g_restartRequested = nullptr;
static unsigned long* g_restartRequestedTime = nullptr;

// SÉCURITÉ: Liste blanche des hôtes autorisés pour les téléchargements OTA
static const char* ALLOWED_OTA_HOSTS[] = {
  "github.com",
  "api.github.com",
  "objects.githubusercontent.com"
};
static const size_t ALLOWED_OTA_HOSTS_COUNT = sizeof(ALLOWED_OTA_HOSTS) / sizeof(ALLOWED_OTA_HOSTS[0]);

void initOtaContext(bool* restartRequested, unsigned long* restartRequestedTime) {
  g_restartRequested = restartRequested;
  g_restartRequestedTime = restartRequestedTime;
  // feature-151 D1 : traçabilité terrain — confirme QUELLE clé publique un
  // firmware donné embarque réellement (empreinte figée au build, pas
  // recalculée ici : voir constants.h, ADR-0098).
  systemLogger.info(String("[feature-151 D1] Clé publique de signature OTA embarquée — "
                           "empreinte SHA-256 (DER) : ") +
                    kOtaSigningPublicKeyFingerprint);
}

static bool isTimeSynchronized() {
  time_t now = time(nullptr);
  return now > 1609459200; // 2021-01-01
}

static bool ensureTimeForTls(AsyncWebServerRequest* request) {
  if (!isTimeSynchronized()) {
    sendErrorResponse(request, 503, "System time not synchronized (NTP requis pour la validation TLS)");
    return false;
  }
  return true;
}

// SÉCURITÉ: Valider que l'URL provient d'un hôte autorisé
static bool isUrlAllowed(const String& url) {
  // L'URL doit commencer par https://
  if (!url.startsWith("https://")) {
    systemLogger.error("URL refusée (non HTTPS): " + url);
    return false;
  }

  // Extraire le nom d'hôte (entre https:// et le premier /)
  int hostStart = 8; // longueur de "https://"
  int hostEnd = url.indexOf('/', hostStart);
  if (hostEnd == -1) {
    hostEnd = url.length();
  }

  String host = url.substring(hostStart, hostEnd);

  // Vérifier si l'hôte est dans la liste blanche
  for (size_t i = 0; i < ALLOWED_OTA_HOSTS_COUNT; i++) {
    if (host == ALLOWED_OTA_HOSTS[i]) {
      return true;
    }
  }

  systemLogger.error("Hôte refusé (non whitelisté): " + host);
  return false;
}

// ============================================================================
// État de l'upload manuel /update (feature-026).
// Le handler d'upload AsyncWebServer est appelé par chunks : l'état doit
// survivre entre les appels. Choix : statiques de fichier réinitialisées à
// index == 0 plutôt que request->_tempObject (pas de new/delete par requête,
// pas de fuite si le client se déconnecte en cours d'upload).
// ATTENTION : la classe `Update` est un singleton global, mais cela n'empêche
// PAS deux requêtes d'upload concurrentes d'atteindre ce handler et de
// corrompre ces statiques (le Update.begin() du 2ᵉ échoue APRÈS que le bloc
// index==0 ait réinitialisé le hasher et effacé l'empreinte attendue).
// D'où le verrou g_uploadOwner : une seule requête propriétaire à la fois,
// toute requête non propriétaire est rejetée sans toucher aux statiques.
// ============================================================================
static OtaStreamHasher g_uploadHasher;          // Hachage incrémental du flux reçu
static size_t g_uploadLastLog = 0;              // Throttle du log de progression (fix AC5)
static uint8_t g_uploadExpected[32];            // Empreinte attendue (param sha256)
static bool g_uploadHasExpected = false;        // Param sha256 fourni ET valide
// feature-151 D1, lot B1 : empreinte DÉCLARÉE par un manifeste PARSÉ (champ
// sha256Hex, 64 hex minuscules + NUL — PAS pré-décodé en binaire : le
// décodage vit dans ota_manifest_binding_logic.cpp, seul endroit autorisé à
// appeler parseOtaSha256Param en dehors du site UNIQUE que la garde
// structurelle G1 impose à web_routes_ota.cpp). ALIMENTÉE à index==0, dans le
// même bloc que le calcul d'authenticité, CONSOMMÉE à `final` pour la
// liaison manifeste ↔ octets réellement écrits. ⚠️ Statique à dessein (comme
// g_uploadExpected) : doit survivre entre les chunks de CETTE requête.
// ⚠️ D3 réutilisera CETTE PAIRE comme SOURCE de g_uploadExpected (ne fera que
// « basculer la source », condition 10 du verdict security-reviewer P1) — ne
// pas la fusionner avec g_uploadExpected ni la renommer sans en tenir compte.
static char g_uploadManifestSha256Hex[65];
static bool g_uploadManifestDigestValid = false;
// g_uploadDigestInvalid a été retiré (feature-118 lot A) : le refus
// « empreinte malformée » est désormais tranché et appliqué à index==0,
// AVANT `g_uploadOwner = request` (voir parseOtaSha256Param) — il n'atteint
// plus jamais le bloc `final`. Lot B : le refus « absente » suit le même
// chemin (même place, même motif) — le bloc `final` ne connaît donc plus que
// CONFORME/NON CONFORME via g_uploadHasExpected (toujours true à ce point).
static AsyncWebServerRequest* g_uploadOwner = nullptr;  // Requête propriétaire de la session d'upload

// --- 2.20.1 : diagnostic d'échec (bug « journal noyé / cause écrasée ») ------
// g_uploadErrorLogged : un échec d'upload ne doit produire QU'UNE ligne de
// journal. Après Update.abort(), Update.write() retourne 0 à chaque chunk
// restant : sans ce garde-fou, une seule tentative ratée écrivait ~200 lignes
// identiques et vidait entièrement l'anneau (kMaxLogEntries = 200), effaçant
// les lignes qui désignaient le coupable.
static bool g_uploadErrorLogged = false;
// Verdict HTTP mémorisé pour la requête d'upload (consommé par le lambda de
// réponse via takeOtaUploadVerdict) : le handler d'upload ne peut pas répondre
// lui-même, mais c'est lui seul qui connaît la CAUSE de l'échec.
static AsyncWebServerRequest* g_uploadVerdictOwner = nullptr;
static int g_uploadVerdictStatus = 0;
static String g_uploadVerdictBody;
// true entre le LittleFS.end() réussi et le remontage : indique que le
// filesystem est démonté DE NOTRE FAIT et doit être remonté sur tout chemin
// terminal (sinon l'UI n'est plus servie jusqu'au prochain reboot).
static bool g_uploadFsUnmounted = false;
// Type de la session en cours : `cmd` est une locale du bloc index==0 qui ne
// survit pas d'un chunk à l'autre, alors que le site d'écriture (chunk N) doit
// pouvoir nommer le type dans son message d'erreur.
static bool g_uploadIsFilesystem = false;

// CONCURRENCE — aucun mutex ici, et c'est VOULU : handleOtaUpdate, le lambda de
// réponse de /update et le callback onDisconnect s'exécutent TOUS sur la même
// tâche AsyncTCP, séquentiellement. Ajouter un verrou n'apporterait rien et
// risquerait d'introduire une attente sur une tâche qui n'est pas abonnée au
// watchdog. Ne pas « corriger » ce point en revue.

// Marque une requête comme rejetée (upload concurrent) via _tempObject :
// marqueur par requête, libéré automatiquement par free() dans le destructeur
// d'AsyncWebServerRequest — pas de fuite même si le client déconnecte.
static void markUploadRejected(AsyncWebServerRequest* request) {
  if (request->_tempObject == nullptr) {
    request->_tempObject = malloc(1);
  }
}

// Mémorise le corps de réponse à servir pour CETTE requête d'upload.
// Le pointeur n'est stocké que comme IDENTITÉ : il n'est jamais déréférencé
// (il peut être pendant si le client s'est déconnecté entre-temps).
static void setOtaUploadVerdict(AsyncWebServerRequest* request, int status, const String& body) {
  g_uploadVerdictOwner = request;
  g_uploadVerdictStatus = status;
  g_uploadVerdictBody = body;
}

/**
 * @brief Récupère et CONSOMME le verdict HTTP mémorisé pour cette requête.
 *
 * Appelé par le lambda de réponse de /update. Le pointeur mémorisé n'est
 * jamais déréférencé, uniquement comparé.
 *
 * @param request Requête qui s'apprête à répondre.
 * @param status  [out] Code HTTP à servir (renseigné seulement si true).
 * @param body    [out] Corps à servir (renseigné seulement si true).
 * @return true si un verdict était mémorisé POUR CETTE requête (slot libéré).
 */
static bool takeOtaUploadVerdict(AsyncWebServerRequest* request, int& status, String& body) {
  if (request == nullptr || g_uploadVerdictOwner != request) {
    return false;
  }
  status = g_uploadVerdictStatus;
  body = g_uploadVerdictBody;
  g_uploadVerdictOwner = nullptr;   // Consommé : une seule réponse par verdict
  g_uploadVerdictStatus = 0;
  g_uploadVerdictBody = String();
  return true;
}

/**
 * @brief Journalise la cause d'un échec d'upload UNE SEULE FOIS et mémorise la
 *        réponse HTTP correspondante.
 *
 * @param request  Requête d'upload concernée.
 * @param status   Code HTTP à servir (400 = image/paramètre refusé, 500 = échec flash).
 * @param cause    Cause en clair, en français, déjà enrichie de l'errorString capturé.
 * @param critical true pour journaliser en CRITICAL (refus d'intégrité).
 */
static void failOtaUpload(AsyncWebServerRequest* request, int status, const String& cause,
                          bool critical = false) {
  if (!g_uploadErrorLogged) {
    g_uploadErrorLogged = true;
    if (critical) {
      systemLogger.critical("Mise à jour OTA refusée : " + cause);
    } else {
      systemLogger.error("Échec mise à jour OTA : " + cause);
    }
  }
  setOtaUploadVerdict(request, status, "FAIL: " + cause);
}

/**
 * @brief Remonte le filesystem s'il a été démonté par la session d'upload.
 *
 * No-op si le FS n'a pas été démonté par nous (mise à jour firmware : il n'a
 * jamais été touché). JAMAIS LittleFS.begin(true) : un formatage détruirait
 * l'UI encore servie aux clients connectés.
 *
 * @param afterFailure true (défaut) si l'on remonte après une mise à jour FS
 *        INTERROMPUE — le contenu de la partition est alors potentiellement
 *        incohérent, ce qui justifie une alerte. false après un flash FS réussi.
 */
static void remountUploadFsIfNeeded(bool afterFailure = true) {
  if (!g_uploadFsUnmounted) {
    return;
  }
  // Drapeau remis à false quel que soit le résultat : une seule tentative de
  // remontage par session, pas de boucle de montage sur image corrompue.
  g_uploadFsUnmounted = false;
  if (LittleFS.begin(false)) {
    if (afterFailure) {
      systemLogger.critical("Filesystem remonté après une mise à jour FS interrompue : "
                            "contenu potentiellement incohérent, relancer la mise à jour");
    } else {
      systemLogger.info("Filesystem remonté après la mise à jour");
    }
  } else {
    systemLogger.warning("Impossible de remonter le filesystem après la mise à jour "
                         "— redémarrage requis");
  }
}

// =============================================================================
// Garde d'authentification du handler d'UPLOAD de POST /update (bug-117, 2.46.2)
// =============================================================================
// PIÈGE — MÊME TOPOLOGIE que les défauts corrigés sur /save-config (v2.25.0,
// saveConfigBodyAuthorized) et sur les trois routes de calibration (v2.46.1,
// calibrationBodyAuthorized) : ESPAsyncWebServer exécute le handler d'UPLOAD
// AVANT le lambda de requête qui porte REQUIRE_AUTH. Le relecteur qui voit
// « REQUIRE_AUTH(req, RouteProtection::CRITICAL) » en tête de setupOtaRoutes()
// croit la route protégée, alors que handleOtaUpdate a déjà appelé
// Update.begin() puis Update.end(true) : l'image est FLASHÉE quand le 401 part.
// Le pré-contrôle de l'octet magique ne compense pas — il contrôle la VALIDITÉ
// de l'image, pas l'AUTORISATION de l'appelant. L'empreinte SHA-256 non plus,
// MÊME devenue obligatoire (feature-118 lot B) : une empreinte fournie par
// l'appelant prouve l'intégrité du TRANSPORT (l'image reçue est celle
// envoyée), jamais l'AUTORISATION de l'appelant ni l'authenticité de l'image
// (qui l'a produite) — voir ADR-0096.
//
// CETTE GARDE N'EST PAS REDONDANTE AVEC REQUIRE_AUTH, et elle ne le remplace
// pas : REQUIRE_AUTH produit le 401 propre (une seule réponse, un seul décompte
// de quota), la garde empêche l'EFFET DE BORD. Supprimer l'une ou l'autre
// réouvre le trou. Ne pas « simplifier » ce point en revue.
//
// ÉQUIVALENCE avec REQUIRE_AUTH(CRITICAL) — dans les deux sens, sinon l'appareil
// devient non déployable (l'OTA est son SEUL canal : pas de port série) :
//   checkAuth(CRITICAL) accepte  ⇔  (jeton valide) OU (basic valide)
//     · l'exemption RouteProtection::NONE ne concerne pas /update ;
//     · l'exemption « premier démarrage » de checkAuth() est bornée aux URL
//       commençant par /save-config — elle n'a jamais couvert /update, donc son
//       absence ici n'ajoute AUCUNE restriction (et l'ajouter serait un trou) ;
//     · checkRateLimit() est volontairement absent (cf. ci-dessus) : il ne peut
//       que REFUSER, jamais accepter — l'omettre élargit, ne restreint pas.
//   Les deux méthodes que ota_update.sh peut employer passent donc la garde :
//     · `curl -u admin:<mdp>`      → checkBasicAuth  → request->authenticate()
//       lit _authorization, renseigné au PARSE DES EN-TÊTES ;
//     · `curl -H 'X-Auth-Token: …'` → checkTokenAuth → request->getHeader(),
//       lu dans _headers, renseigné au PARSE DES EN-TÊTES.
//   L'interface web (data/app.js, xhr.setRequestHeader('X-Auth-Token')) emprunte
//   le second chemin.
//
// DISPONIBILITÉ DES EN-TÊTES AU PREMIER FRAGMENT — vérifié sur ESPAsyncWebServer
// 3.11.2 (le supposer ne suffisait pas) : _parseReqHeader() renseigne
// _authorization/_authMethod et empile chaque en-tête dans _headers pendant
// l'état PARSE_REQ_HEADERS ; _parseMultipartPostByte() → _handleUploadByte() ne
// tourne qu'ensuite, et RIEN ne purge _headers avant la fin de la requête. Les
// deux méthodes sont donc lisibles dès le fragment index == 0 et le restent
// jusqu'au fragment final. (Le commentaire « check if authenticated before
// calling the upload » de WebRequest.cpp:738 n'est suivi d'AUCUN contrôle en
// amont : la responsabilité est bien à notre charge.)
//
// ORDRE DES DEUX PRÉDICATS — inversé par rapport à checkAuth() (basic d'abord),
// et le court-circuit hasHeader() est délibéré : cette garde s'exécute à CHAQUE
// fragment (≈1000 fois pour un firmware de 1,5 Mo), or checkTokenAuth() émet
// `systemLogger.debug("Aucun header X-Auth-Token trouvé")` quand l'en-tête est
// absent — soit, pour un upload en authentification basique, une String
// construite et un millier de lignes de journal PENDANT le flash. L'ensemble
// accepté est identique : checkTokenAuth() ne peut retourner true QUE si
// l'en-tête existe, donc « en-tête absent » ⇒ false dans les deux écritures.
static bool otaUploadAuthorized(AsyncWebServerRequest* request) {
  if (request == nullptr) return false;                 // fail-closed
  if (!authManager.isEnabled()) return true;            // authentification désactivée
  if (authManager.checkBasicAuth(request)) return true;  // curl -u admin:<mdp>
  // Court-circuit STRICTEMENT équivalent : checkTokenAuth() ne lit que cet en-tête et
  // refuse délibérément le paramètre d'URL (motifs écrits dans auth.cpp). Il évite la
  // String de son systemLogger.debug() quand l'en-tête manque — ~1000 fragments par
  // flash de 1,5 Mo.
  // ⚠️ La justification initiale de l'ORDRE des prédicats était INVERSÉE (corrigée
  // après revue, 2026-09-07) : authenticate() court-circuite déjà sur _authorization
  // vide, donc le chemin JETON n'allouait rien de toute façon. C'est le chemin BASIC
  // — celui d'ota_update.sh par défaut — qui paie generateBasicHash (2 new char[] +
  // String) à CHAQUE fragment. Aucun ordre ne l'évite : la garde est réévaluée par
  // fragment à dessein (un pointeur réutilisé après free accorderait l'auth).
  // Allocations transitoires et LIFO, jugées non bloquantes en revue — mais c'est un
  // coût assumé, pas un coût évité.
  if (!request->hasHeader("X-Auth-Token")) return false;
  return authManager.checkTokenAuth(request);            // curl -H 'X-Auth-Token: …'
}

// IP d'origine, sans jamais déréférencer un client nul : le client peut avoir
// disparu entre l'arrivée du fragment et la journalisation — un log ne doit pas
// faire tomber l'appareil.
static String otaClientIpOf(AsyncWebServerRequest* request) {
  if (request == nullptr || request->client() == nullptr) return "IP inconnue";
  return request->client()->remoteIP().toString();
}

// Journalisation THROTTLÉE du refus (1 WARN/min pour ce site).
// MOTIF : le quota kMaxRequestsPerMinute ne borne PAS cet émetteur — checkRateLimit()
// est appelé par checkAuth() dans le handler de REQUÊTE, donc APRÈS l'upload ; le
// handler d'upload tourne pour chaque requête reçue, quota dépassé ou non. Sans
// throttle, un émetteur non authentifié purge les 6 144 o conservés du journal
// (kRotateKeepBytes, logger.h:50-51) en ~27 requêtes soit ~13 s, effaçant la fenêtre
// forensique dont dépend bug-080.
// ⚠️ MOTIF SUREVALUÉ — corrigé après revue (2026-09-07) : ce throttle réduit la
// pression, il ne la supprime PAS. sendAuthRequired() (auth.cpp:234) émet déjà, dans
// le lambda de réponse, UN WARN NON THROTTLÉ par requête sur cette route. La fenêtre
// forensique reste donc purgeable sans authentification, et ce throttle-ci n'y change
// rien à lui seul. Le sujet est l'émetteur GÉNÉRIQUE : voir bug-112. Ne pas conclure
// de la présence de ce throttle que le vecteur est fermé.
// Seuil RÉUTILISÉ tel quel : kCalibrationAuthWarnThrottleMs (60 s). Même valeur, même
// motif et même chiffrage documenté dans constants.h — introduire une constante jumelle
// n'aurait fait que dupliquer la démonstration. Le nom mériterait d'être généralisé
// (kBodyAuthWarnThrottleMs) le jour où constants.h sera rouvert.
// INVARIANT : quand le site est muet, AUCUNE String n'est construite — sortie avant
// toute concaténation. Ce chemin est atteignable sans authentification.
static void logUnauthorizedOtaUpload(AsyncWebServerRequest* request) {
  static unsigned long lastWarnMs = 0;
  static uint16_t suppressedCount = 0;

  const unsigned long now = millis();
  // lastWarnMs == 0 == « jamais journalisé » : sans ce cas, la première tentative des
  // 60 premières secondes de vie de l'appareil serait avalée. La comparaison par
  // soustraction non signée reste immunisée au repliement de millis() (49,7 j).
  if (lastWarnMs != 0 && (now - lastWarnMs) < kCalibrationAuthWarnThrottleMs) {
    if (suppressedCount < UINT16_MAX) suppressedCount++;
    return;  // MUET : zéro allocation
  }

  String msg = String("[OTA] POST /update NON AUTHENTIFIÉ depuis ") + otaClientIpOf(request) +
               " — upload ignoré (aucun Update.begin(), aucune écriture de partition)";
  if (suppressedCount > 0) {
    msg += " ; et " + String(suppressedCount) + " tentatives non journalisées";
    suppressedCount = 0;
  }
  systemLogger.warning(msg);
  lastWarnMs = now;
}

// feature-118 lot B : trace, pour CHAQUE requête d'upload, le verdict que le
// prédicat pur rend sur l'empreinte sha256 — la décision (refus ou non) est
// désormais RÉELLE, cf. otaUploadIntegrityIsRejected, appliquée par les
// appelants de cette fonction (branche « absente » à index==0, branche « non
// conforme » à `final`). Niveau WARNING réservé aux anomalies (malformée et
// absente : refusées ; non conforme : refusée) — INFO sinon (conforme : rien
// à signaler).
static void logOtaUploadIntegrityVerdict(OtaUploadIntegrityVerdict verdict) {
  const bool rejected = otaUploadIntegrityIsRejected(verdict);
  const String msg = String("[feature-118 lot B] Empreinte upload OTA : ") +
                     otaUploadIntegrityVerdictLabel(verdict) +
                     " — refusée : " + (rejected ? "oui" : "non");
  if (rejected) {
    systemLogger.warning(msg);
  } else {
    systemLogger.info(msg);
  }
}

// =============================================================================
// feature-151 D1 : vérification d'AUTHENTICITÉ — JOURNALISE SEULEMENT.
// =============================================================================
// ⛔ Cette fonction ne doit JAMAIS produire de refus — Q0 du verdict
// security-reviewer P1 : le chemin d'ACCEPTATION doit être prouvé sur le
// terrain (D2) AVANT que le chemin de REFUS n'existe (D3). D3 réutilisera
// cette MÊME fonction à l'identique ; seul l'appelant ajoutera un
// `if (otaAuthenticityIsRejected(verdict))` (condition 10 : octet pour octet
// identique entre D1 et D3 sur le vérificateur, la clé, la grammaire).
//
// Paramètres attendus (form-data, mêmes conventions que le `sha256` existant
// de feature-118) : `manifest` (texte brut "pool-ota/1 ...") et `signature`
// (base64 standard, 512 caractères sans padding pour RSA-3072). Absents sur
// le chemin normal aujourd'hui (aucun outillage ne les envoie encore côté
// page web, hors périmètre de ce lot) : verdict kAbsent, journalisé en INFO,
// sans aucune conséquence.
//
// Résultat complet de computeOtaAuthenticityVerdict() — le verdict seul ne
// suffit plus depuis le lot B1 (revue de sécurité du 2026-10-18) : l'appelant
// a aussi besoin (a) des champs du manifeste PARSÉ, pour la liaison
// manifeste ↔ image (voir ota_manifest_binding_logic.h), et (b) des octets
// canoniques re-sérialisés, pour diagnostiquer en journal un refus sans
// jamais exposer de secret (le manifeste est public — M2, revue du
// 2026-10-18). Struct plutôt qu'une pile d'arguments de sortie : copie
// uniquement sur la pile, aucune allocation.
struct OtaAuthenticityComputation {
  OtaAuthenticityVerdict verdict = OtaAuthenticityVerdict::kAbsent;
  OtaManifestFields fields{};
  bool fieldsValid = false;              // true seulement si parseOtaManifest == kOk
  char canonical[kOtaManifestMaxLen] = {0};
  size_t canonicalLen = 0;               // 0 si jamais calculé (parse ou décodage signature en échec)
};

static OtaAuthenticityComputation computeOtaAuthenticityVerdict(bool manifestPresent,
                                                                bool signaturePresent,
                                                                const char* manifestRaw,
                                                                const char* signatureRaw,
                                                                OtaManifestKind expectedKind) {
  OtaAuthenticityComputation result;
  OtaManifestFields fields{};
  OtaManifestParseVerdict parseVerdict = OtaManifestParseVerdict::kMalformed;
  bool signatureDecodeOk = false;
  OtaCryptoVerdict cryptoVerdict = OtaCryptoVerdict::kImpossible;

  if (manifestPresent && signaturePresent) {
    parseVerdict = parseOtaManifest(manifestRaw, &fields);

    uint8_t signatureBytes[kOtaSignatureMaxBytes];
    const size_t signatureLen =
        otaBase64Decode(signatureRaw, strlen(signatureRaw), signatureBytes, sizeof(signatureBytes));
    // RSA-3072 → EXACTEMENT kOtaSignatureMaxBytes octets décodés, jamais moins.
    signatureDecodeOk = (signatureLen == kOtaSignatureMaxBytes);

    if (parseVerdict == OtaManifestParseVerdict::kOk && signatureDecodeOk) {
      // Re-sérialisation CANONIQUE depuis les champs parsés — la signature
      // porte sur CES octets, jamais sur `manifestRaw` tel que reçu.
      const size_t canonicalLen =
          serializeOtaManifest(fields, result.canonical, sizeof(result.canonical));
      if (canonicalLen > 0) {
        cryptoVerdict = otaVerifySignature(reinterpret_cast<const uint8_t*>(result.canonical),
                                           canonicalLen, signatureBytes, signatureLen);
        result.canonicalLen = canonicalLen;
      }
      // canonicalLen == 0 (buffer insuffisant) ne devrait jamais survenir vu
      // kOtaManifestMaxLen — cryptoVerdict reste kImpossible par défaut.
    }
  }

  // Champs exposés DÈS que le manifeste PARSE (indépendamment du verdict
  // cryptographique) : la liaison manifeste ↔ image (B1) est une question
  // DIFFÉRENTE de l'authenticité de la signature, et doit rester observable
  // même si la signature est invalide — pour en distinguer la cause en
  // journal (une image non liée à un manifeste par ailleurs invalide n'est
  // pas plus grave qu'un manifeste déjà rejeté pour une autre raison).
  if (parseVerdict == OtaManifestParseVerdict::kOk) {
    result.fields = fields;
    result.fieldsValid = true;
  }

  result.verdict = combineOtaAuthenticityVerdict(
      manifestPresent, signaturePresent, parseVerdict, signatureDecodeOk, cryptoVerdict,
      fields.kind, expectedKind, fields.major, fields.minor, fields.patch,
      kOtaVersionFloorDefaultMajor, kOtaVersionFloorDefaultMinor, kOtaVersionFloorDefaultPatch);
  return result;
}

// Journalisation du verdict d'authenticité — WARNING si D3 aurait refusé,
// INFO sinon. Le texte rappelle EXPLICITEMENT qu'aucun refus n'est appliqué
// (D1) : un opérateur qui lirait seulement le niveau WARNING ne doit pas
// croire à tort qu'un flash a été bloqué.
//
// M2 (revue de sécurité, 2026-10-18) : le piège du LF final (un manifeste
// signé via `echo` au lieu de `printf`, cf. ota_update.sh) produit un verdict
// kInvalid STRICTEMENT indistinguable en apparence d'une mauvaise clé — alors
// que c'est l'échec d'outillage le plus probable d'une première signature
// manuelle. Dès que `canonical` a été calculé (parse + décodage signature
// réussis), ces octets sont journalisés : le manifeste circule déjà en clair
// dans la requête HTTP, ce n'est PAS un secret — les exposer permet à
// l'opérateur de les comparer en quelques secondes à ce que son `printf` a
// réellement signé.
static void logOtaAuthenticityVerdict(const char* routeName,
                                      const OtaAuthenticityComputation& computation) {
  const bool wouldReject = otaAuthenticityIsRejected(computation.verdict);
  const String msg = String("[feature-151 D1] ") + routeName + " — authenticité OTA : " +
                     otaAuthenticityVerdictLabel(computation.verdict) +
                     " — D3 refuserait : " + (wouldReject ? "oui" : "non") +
                     " — AUCUN refus appliqué (D1, vérification seule)";
  if (wouldReject) {
    systemLogger.warning(msg);
    if (computation.canonicalLen > 0) {
      systemLogger.warning(String("[feature-151 D1] ") + routeName +
                           " — octets canoniques re-sérialisés par l'appareil (comparer avec "
                           "ce qui a été signé) : \"" + computation.canonical + "\"");
    }
  } else {
    systemLogger.info(msg);
  }
}

// =============================================================================
// feature-151 D1, lot B1 : liaison manifeste ↔ image — JOURNALISE SEULEMENT.
// =============================================================================
// ⛔ Même principe que logOtaAuthenticityVerdict : AUCUN refus. Un verdict
// d'authenticité « valide » prouve que LE MANIFESTE a été signé par la bonne
// clé — jamais que l'image reçue est CELLE qu'il décrit (B1, revue de
// sécurité du 2026-10-18 : sans cette liaison, un manifeste signé pour
// l'image A, envoyé avec l'image B et un paramètre sha256/digest=<B>,
// obtenait un verdict d'authenticité « valide »).
static void logOtaManifestBindingVerdict(const char* routeName, const char* stage,
                                         OtaManifestBindingVerdict verdict) {
  const bool divergent = otaManifestBindingIsDivergent(verdict);
  const String msg = String("[feature-151 D1 B1] ") + routeName + " — liaison manifeste/image (" +
                     stage + ") : " + otaManifestBindingVerdictLabel(verdict) +
                     " — D3 refuserait : " + (divergent ? "oui" : "non") +
                     " — AUCUN refus appliqué (D1, vérification seule)";
  if (divergent) {
    systemLogger.warning(msg);
  } else {
    systemLogger.info(msg);
  }
}

static void handleOtaUpdate(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  // GARDE D'AUTHENTIFICATION — PREMIÈRE INSTRUCTION, avant tout effet de bord
  // (bug-117). Évaluée à CHAQUE fragment, pas seulement à index == 0 : un client
  // ne doit pas pouvoir franchir la garde une fois puis pousser la suite du flux.
  // Rien n'est touché sur ce chemin — aucune statique, pas de g_uploadOwner, pas
  // de markUploadRejected : c'est REQUIRE_AUTH, dans le lambda de réponse, qui
  // émettra le 401.
  if (!otaUploadAuthorized(request)) {
    if (!index) {
      logUnauthorizedOtaUpload(request);
    }
    return;
  }

  if (!index) {
    // Verrou anti-uploads concurrents : si une autre requête possède déjà la
    // session, la refuser AVANT de toucher aux statiques (sinon le 2ᵉ upload
    // réinitialiserait le hasher et effacerait l'empreinte attendue du 1ᵉʳ).
    if (g_uploadOwner != nullptr && g_uploadOwner != request) {
      if (Update.isRunning()) {
        systemLogger.error("Upload OTA concurrent refusé: une mise à jour est déjà en cours");
        markUploadRejected(request);
        return;  // Ne touche à AUCUNE statique — la session du 1ᵉʳ reste intacte
      }
      // Owner fantôme : le client précédent a déconnecté sans que le bloc
      // final n'arrive (le nettoyage onDisconnect devrait l'avoir libéré,
      // ceinture-bretelles ici). Update n'est pas en cours → aucune session
      // active à protéger, on reprend la main.
      systemLogger.warning("Upload OTA: session propriétaire périmée détectée, reprise par la nouvelle requête");
    }

    // Réinitialisation ANTICIPÉE du garde-fou anti-flood : le refus de type
    // ci-dessous doit pouvoir journaliser sa cause même si la tentative
    // PRÉCÉDENTE s'était soldée par une erreur (le bloc de réinitialisation
    // complet, lui, n'intervient qu'après la prise de propriété).
    g_uploadErrorLogged = false;

    // ---- Détermination du type AVANT le moindre effet de bord (2.20.1) -----
    // Déplacé ici depuis l'après-Update.begin() : le pré-contrôle de l'octet
    // magique doit pouvoir refuser l'image AVANT setOtaInProgress(true),
    // AVANT LittleFS.end() et AVANT Update.begin().
    int cmd = U_FLASH; // Par défaut: firmware

    // Priorité 1: Utiliser le paramètre update_type du formulaire si disponible
    if (request->hasParam("update_type", true)) {
      String updateType = request->getParam("update_type", true)->value();
      if (updateType == "filesystem") {
        cmd = U_SPIFFS;
        systemLogger.info("Type de mise à jour: Filesystem (paramètre formulaire)");
      } else {
        systemLogger.info("Type de mise à jour: Firmware (paramètre formulaire)");
      }
    }
    // Priorité 2: Détecter par le nom de fichier si pas de paramètre.
    // inferOtaKindFromFilename reconnaît désormais le nom NU `littlefs.bin`
    // (produit par build_fs.sh), que les anciens suffixes rataient.
    else if (inferOtaKindFromFilename(filename.c_str()) == OtaImageKind::kFilesystem) {
      cmd = U_SPIFFS;
      systemLogger.info("Type de mise à jour: Filesystem (détection nom fichier)");
    } else {
      systemLogger.info("Type de mise à jour: Firmware (détection nom fichier)");
    }

    // ---- feature-151 D1 : vérification d'AUTHENTICITÉ — JOURNALISE SEULEMENT --
    // Placée ici à dessein : AVANT le pré-contrôle de l'octet magique,
    // `g_uploadOwner`, `setOtaInProgress(true)`, `LittleFS.end()` et
    // `Update.begin()` — place DÉFINITIVE pour D3 (Q0 du verdict
    // security-reviewer P1), mais ⛔ AUCUN `return` de refus ici : D1 ne fait
    // que calculer et journaliser le verdict que D3 appliquera.
    // GARDE STRUCTURELLE : test/test_native_ota_order_guard (guard [G5]) fige
    // cette place — ne pas la déplacer sans mettre à jour ce test.
    {
      const OtaManifestKind expectedKind =
          (cmd == U_SPIFFS) ? OtaManifestKind::kFilesystem : OtaManifestKind::kFirmware;
      const bool manifestPresent = request->hasParam("manifest", true);
      const bool signaturePresent = request->hasParam("signature", true);
      const String manifestRaw =
          manifestPresent ? request->getParam("manifest", true)->value() : String();
      const String signatureRaw =
          signaturePresent ? request->getParam("signature", true)->value() : String();
      const OtaAuthenticityComputation authenticity = computeOtaAuthenticityVerdict(
          manifestPresent, signaturePresent, manifestRaw.c_str(), signatureRaw.c_str(),
          expectedKind);
      logOtaAuthenticityVerdict("/update", authenticity);

      // ---- lot B1 : liaison manifeste ↔ image (revue de sécurité, 2026-10-18) --
      // Mémorise le hex DÉCLARÉ (PAS pré-décodé : voir le commentaire de
      // g_uploadManifestSha256Hex plus haut — le décodage vit dans
      // ota_manifest_binding_logic.cpp pour préserver l'unicité de l'appel à
      // parseOtaSha256Param dans CE fichier, garde structurelle G1). Statique
      // à dessein : doit survivre jusqu'au chunk `final`, seul endroit où
      // l'empreinte RÉELLEMENT calculée sur les octets écrits existe
      // (comparaison 2/2, plus bas dans ce fichier). D3 n'aura qu'à BASCULER
      // LA SOURCE de g_uploadExpected vers cette paire — ne pas la réécrire.
      g_uploadManifestDigestValid = authenticity.fieldsValid;
      if (authenticity.fieldsValid) {
        memcpy(g_uploadManifestSha256Hex, authenticity.fields.sha256Hex,
               sizeof(g_uploadManifestSha256Hex));
      }
    }

    // ---- Pré-contrôle de l'octet magique (2.20.1) --------------------------
    // Une image filesystem envoyée en type « firmware » était refusée par
    // UpdateClass::_writeBuffer() APRÈS coup, avec une cause écrasée par
    // abort() et un journal noyé. On la refuse ici, sans aucun effet de bord.
    // Voir la réserve bloquante de src/ota_type_logic.h : équivalence acquise
    // tant que Update.setupCrypt() n'est jamais appelé.
    if (len >= 1) {
      const OtaTypeVerdict verdict = checkOtaFirstByte(cmd == U_FLASH, data[0]);
      if (verdict == OtaTypeVerdict::kRejectNotFirmware) {
        char magicHex[5];
        snprintf(magicHex, sizeof(magicHex), "0x%02X", data[0]);
        // Refus AVANT `g_uploadOwner = request` (pas d'owner fantôme) et avant
        // toute inhibition des pompes / démontage du filesystem.
        // markUploadRejected : sans marqueur par requête, Update.hasError()
        // serait faux → réponse "OK" et REDÉMARRAGE PARASITE.
        markUploadRejected(request);
        failOtaUpload(request, 400,
                      String("l'image reçue n'est pas un firmware ESP32 (octet magique ") +
                          magicHex + " au lieu de 0xE9) — type « Système de fichiers » attendu ?");
        // Ce chemin sort AVANT la prise de propriété, donc avant l'onDisconnect
        // de session : sans ce nettoyage dédié, un client qui coupe avant de
        // lire la réponse laisserait g_uploadVerdictOwner pendant, et un futur
        // AsyncWebServerRequest réalloué à la même adresse consommerait ce
        // verdict. Le pointeur est UNIQUEMENT comparé, jamais déréférencé.
        request->onDisconnect([request]() {
          if (g_uploadVerdictOwner == request) {
            g_uploadVerdictOwner = nullptr;
            g_uploadVerdictStatus = 0;
            g_uploadVerdictBody = String();
          }
        });
        return;
      }
      if (verdict == OtaTypeVerdict::kWarnLooksLikeFirmware) {
        // Jamais de refus dans ce sens : LittleFS n'a pas de magique fiable à
        // l'offset 0, un 0xE9 fortuit y est possible → on avertit et on poursuit.
        systemLogger.warning("Mise à jour « Système de fichiers » : l'image reçue commence par "
                             "l'octet magique firmware 0xE9 — vérifier le type sélectionné");
      }
    }

    // ---- feature-118 lot A/B : paramètre `sha256` — parse + refus « malformée » et « absente » (C1, C2, C3) --
    // HORS du `if (len >= 1)` ci-dessus à dessein : un fichier VIDE produit un
    // unique appel `len==0, final==true`, et le paramètre `sha256` (form-data)
    // est déjà disponible à ce stade (en-têtes + champs non-fichier parsés
    // avant le flux fichier, cf. le commentaire bug-117 plus haut) — il ne
    // doit pas dépendre de la présence d'octets de fichier pour être contrôlé.
    // `shaParamVerdict` est une variable LOCALE (pas de static requis : tout
    // son usage — ici et dans le bloc de réarmement de g_uploadHasExpected
    // plus bas — reste dans CETTE MÊME invocation à index==0) servant DEUX
    // FOIS : ici (refus immédiat si malformé) et plus bas (armement de la
    // comparaison de fin, cf. `final`).
    const bool shaPresent = request->hasParam("sha256", true);
    String shaProvided;
    if (shaPresent) {
      shaProvided = request->getParam("sha256", true)->value();
    }
    const OtaSha256ParamVerdict shaParamVerdict =
        parseOtaSha256Param(shaPresent, shaPresent ? shaProvided.c_str() : nullptr,
                            g_uploadExpected);

    if (shaParamVerdict == OtaSha256ParamVerdict::kMalformed) {
      // MÊME MOTIF que l'ancien refus de fin d'upload (feature-026), juste
      // DÉPLACÉ plus tôt (condition C1, security-reviewer P1) : aucun
      // Update.begin() n'a encore eu lieu à ce stade, donc aucun
      // Update.abort() à faire ici — à la différence du refus final
      // d'origine qu'il remplace.
      logOtaUploadIntegrityVerdict(OtaUploadIntegrityVerdict::kMalformed);
      // Refus AVANT `g_uploadOwner = request` (pas d'owner fantôme) et avant
      // toute inhibition des pompes / démontage du filesystem.
      // markUploadRejected : sans marqueur par requête, Update.hasError()
      // serait faux → réponse "OK" et REDÉMARRAGE PARASITE.
      markUploadRejected(request);
      failOtaUpload(request, 400,
                    "empreinte SHA-256 fournie invalide — flash refusé", true);
      // Même nettoyage onDisconnect que le refus de type ci-dessus : ce
      // chemin sort AVANT la prise de propriété, donc avant l'onDisconnect
      // de session.
      request->onDisconnect([request]() {
        if (g_uploadVerdictOwner == request) {
          g_uploadVerdictOwner = nullptr;
          g_uploadVerdictStatus = 0;
          g_uploadVerdictBody = String();
        }
      });
      return;
    }

    // feature-118 LOT B : refus « absente » — même place, même motif, MÊME
    // SÉQUENCE que le refus « malformée » ci-dessus (copié à l'identique,
    // security-reviewer P1 condition 1) : AVANT `g_uploadOwner = request`,
    // HORS du `if (len >= 1)` (un fichier VIDE produit un unique appel
    // `len==0, final==true` — le paramètre `sha256` ne doit pas dépendre de
    // la présence d'octets de fichier pour être contrôlé). Aucun
    // setOtaInProgress(true), LittleFS.end() ni Update.begin() n'est
    // atteignable sur ce chemin.
    // ⚠️ NE PAS attendre `final` : l'absence est connue dès index==0 —
    // attendre effacerait la partition (LittleFS.end()/esp_partition) pour
    // une requête déjà condamnée.
    if (shaParamVerdict == OtaSha256ParamVerdict::kAbsent) {
      logOtaUploadIntegrityVerdict(OtaUploadIntegrityVerdict::kAbsent);
      // markUploadRejected : sans marqueur par requête, Update.hasError()
      // serait faux → réponse "OK" et REDÉMARRAGE PARASITE.
      markUploadRejected(request);
      failOtaUpload(request, 400, kOtaSha256MissingMessage, true);
      // Même nettoyage onDisconnect que les deux refus précédents : ce
      // chemin sort AVANT la prise de propriété, donc avant l'onDisconnect
      // de session.
      request->onDisconnect([request]() {
        if (g_uploadVerdictOwner == request) {
          g_uploadVerdictOwner = nullptr;
          g_uploadVerdictStatus = 0;
          g_uploadVerdictBody = String();
        }
      });
      return;
    }

    // feature-151 D1, lot B1 : liaison manifeste ↔ image — comparaison 1/2, au
    // paramètre `sha256` déjà PARSÉ ci-dessus (g_uploadExpected). Placée ICI,
    // après les deux `return` malformée/absente : shaParamVerdict ne peut plus
    // valoir que kParsed à ce point (même garantie que le commentaire plus bas
    // sur g_uploadHasExpected), donc g_uploadExpected est rempli. AUCUN nouvel
    // appel à parseOtaSha256Param ici — réemploi direct du résultat déjà
    // calculé (la garde structurelle G1 exige que cet appel reste un site
    // UNIQUE dans ce fichier). ⛔ JOURNALISE SEULEMENT.
    logOtaManifestBindingVerdict(
        "/update", "paramètre sha256 (index==0)",
        otaManifestBindingVerdict(g_uploadManifestDigestValid, g_uploadManifestSha256Hex,
                                  /*comparedDigestValid=*/true, g_uploadExpected));

    g_uploadOwner = request;

    // Si le client déconnecte en plein transfert, le bloc final n'arrive
    // jamais : libérer le verrou et annuler l'OTA pour ne pas bloquer les
    // uploads suivants (sinon owner fantôme jusqu'au reboot).
    request->onDisconnect([request]() {
      if (g_uploadOwner == request) {
        systemLogger.warning("Upload OTA: client déconnecté en cours de transfert — session annulée");
        if (Update.isRunning()) {
          Update.abort();
        }
        // ORDRE IMPOSÉ : libérer le dosage AVANT tout montage LittleFS.
        PumpController.setOtaInProgress(false);
        remountUploadFsIfNeeded();
        g_uploadOwner = nullptr;
      }
      // Nettoyage HORS du garde ci-dessus : un échec survenu APRÈS la prise de
      // propriété a déjà remis g_uploadOwner à nullptr tout en laissant un
      // verdict en attente. Si le client se déconnecte avant de le lire, ce
      // slot resterait un pointeur pendant. Comparaison seule, jamais de
      // déréférencement.
      if (g_uploadVerdictOwner == request) {
        g_uploadVerdictOwner = nullptr;
        g_uploadVerdictStatus = 0;
        g_uploadVerdictBody = String();
      }
    });

    // Réinitialiser TOUT l'état statique de l'upload (AC5 : deux tentatives
    // successives sans reboot ne doivent partager aucun état). Remonté avant
    // les effets de bord pour qu'aucun résidu de la tentative précédente
    // (empreinte, verdict, garde anti-flood) ne survive.
    // NOTE : g_uploadFsUnmounted n'est VOLONTAIREMENT pas remis à false ici —
    // il reflète l'état réel du montage, pas l'état de la session ; le forcer
    // à false laisserait un filesystem démonté pour de bon si un chemin
    // terminal avait été raté.
    g_uploadLastLog = 0;
    g_uploadHasExpected = false;
    g_uploadErrorLogged = false;
    g_uploadVerdictOwner = nullptr;
    g_uploadVerdictStatus = 0;
    g_uploadVerdictBody = String();
    g_uploadIsFilesystem = (cmd == U_SPIFFS);
    g_uploadHasher.begin();

    systemLogger.info("Début mise à jour OTA: " + filename);
    PumpController.setOtaInProgress(true);

    // Param POST `sha256` OBLIGATOIRE depuis feature-118 lot B. Déjà tranché
    // et PARSÉ à index==0, AVANT la prise de propriété (shaParamVerdict,
    // g_uploadExpected) : MALFORMED et ABSENT sont déjà sortis par un
    // `return` plus haut et ne peuvent plus être observés ici — seul PARSED
    // atteint ce point, donc cette affectation vaut toujours true. Conservée
    // explicite (plutôt que `g_uploadHasExpected = true;`) : elle documente
    // le CONTRAT (shaParamVerdict ne peut être que kParsed ici) sans dupliquer
    // la logique de décision, qui reste dans parseOtaSha256Param/le refus
    // ci-dessus — une seule source de vérité.
    g_uploadHasExpected = (shaParamVerdict == OtaSha256ParamVerdict::kParsed);

    // Démonter LittleFS avant la mise à jour du filesystem
    if (cmd == U_SPIFFS) {
      LittleFS.end();
      g_uploadFsUnmounted = true;  // Doit être remonté sur tout chemin terminal
      systemLogger.info("LittleFS démonté pour mise à jour manuelle");
    }

    // Démarrer la mise à jour
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      // errorString() capturé AVANT toute autre opération sur Update.
      const String beginError = String(Update.errorString());
      failOtaUpload(request, 500,
                    String("démarrage de la mise à jour impossible (") + beginError + ")");
      // Chemin terminal : réarmer le dosage (sinon pompes inhibées jusqu'au
      // reboot — le final est sauté par la garde owner) puis remonter le FS.
      // ORDRE IMPOSÉ : setOtaInProgress(false) AVANT LittleFS.begin().
      PumpController.setOtaInProgress(false);
      remountUploadFsIfNeeded();
      g_uploadOwner = nullptr;
      return;  // Arrêter immédiatement — Update.write() échouerait en cascade sinon
    }
  }

  // Requête non propriétaire (upload concurrent rejeté à index==0, ou begin
  // échoué) : ignorer ses chunks et son final sans toucher à la session active.
  if (request != g_uploadOwner) {
    return;
  }

  // Écrire les données
  if (len) {
    // Hachage incrémental du flux reçu (feature-026) — O(len) par chunk,
    // accélération matérielle mbedtls, non bloquant pour le handler async.
    g_uploadHasher.update(data, len);

    if (Update.write(data, len) != len) {
      // CAUSE CAPTURÉE AVANT abort() : _abort(UPDATE_ERROR_ABORT) remplace le
      // code d'erreur réel (MAGIC_BYTE, ERASE, WRITE, SPACE…) — après abort()
      // la vraie cause est perdue à jamais, y compris pour la finalisation.
      const String writeError = String(Update.errorString());
      Update.abort();
      failOtaUpload(request, 500,
                    String("échec d'écriture de l'image ") +
                        (g_uploadIsFilesystem ? "« Système de fichiers »" : "« Firmware »") +
                        " (" + writeError + ")");
      // Chemin TERMINAL : on relâche la session ici. Sans cela, Update.write()
      // retournerait 0 sur chacun des chunks restants et re-journaliserait la
      // même erreur ~200 fois, vidant tout l'anneau du journal.
      // ORDRE IMPOSÉ : setOtaInProgress(false) AVANT LittleFS.begin().
      PumpController.setOtaInProgress(false);
      remountUploadFsIfNeeded();
      g_uploadOwner = nullptr;
      return;
    } else {
      // Log progression toutes les 100KB (g_uploadLastLog réinitialisé à index==0)
      if (index - g_uploadLastLog >= 102400) {
        unsigned int percent = (index + len) * 100 / Update.size();
        systemLogger.info("Progression OTA: " + String(percent) + "%");
        g_uploadLastLog = index;
      }
    }
  }

  // Finaliser
  if (final) {
    // Empreinte SHA-256 de l'image reçue : toujours calculée et loggée
    // (traçabilité même sans empreinte fournie — comportement documenté).
    uint8_t computed[32];
    g_uploadHasher.finish(computed);
    char computedHex[kOtaSha256HexLen + 1];
    sha256ToHex(computed, computedHex, sizeof(computedHex));
    systemLogger.info("Empreinte SHA-256 de l'image reçue: " + String(computedHex));

    // feature-151 D1, lot B1 : comparaison 2/2 — l'empreinte DÉCLARÉE par le
    // manifeste (si un manifeste authentifiable a été reçu à index==0) contre
    // l'empreinte RÉELLEMENT calculée sur les octets écrits en flash. C'est
    // ICI, et seulement ici, que la liaison manifeste ↔ image peut être
    // tranchée avec certitude (comparaison 1/2 plus haut ne portait que sur le
    // paramètre déclaré par l'appelant, pas sur ce qui a été effectivement
    // écrit). ⛔ JOURNALISE SEULEMENT.
    logOtaManifestBindingVerdict(
        "/update", "empreinte calculée (final)",
        otaManifestBindingVerdict(g_uploadManifestDigestValid, g_uploadManifestSha256Hex,
                                  /*comparedDigestValid=*/true, computed));

    // feature-118 lot B : verdict d'authenticité tracé ici — la DÉCISION pour
    // NON CONFORME a lieu juste en dessous (comparaison, déjà en vigueur
    // depuis feature-026). MALFORMED et ABSENT n'atteignent plus JAMAIS ce
    // point : tous deux refusés à index==0 (ci-dessus), avant même que
    // `computed` existe — g_uploadHasExpected vaut donc TOUJOURS true ici
    // (seul kParsed a pu survivre jusqu'à `final`). Le ternaire est conservé
    // tel quel (defensive, ne dérive pas du contrat) plutôt que simplifié en
    // une constante : aucun refactor opportuniste sur ce chemin critique.
    {
      const OtaSha256ParamVerdict paramVerdictAtFinal =
          g_uploadHasExpected ? OtaSha256ParamVerdict::kParsed : OtaSha256ParamVerdict::kAbsent;
      logOtaUploadIntegrityVerdict(
          otaUploadIntegrityVerdict(paramVerdictAtFinal, g_uploadExpected, computed));
    }

    // Empreinte attendue (désormais TOUJOURS fournie, lot B) → comparaison
    // AVANT Update.end() : en cas de mismatch l'image n'est jamais activée.
    if (g_uploadHasExpected && !sha256Equal(computed, g_uploadExpected)) {
      char expectedHex[kOtaSha256HexLen + 1];
      sha256ToHex(g_uploadExpected, expectedHex, sizeof(expectedHex));
      Update.abort();
      failOtaUpload(request, 400,
                    String("empreinte SHA-256 non conforme (attendue ") + expectedHex +
                        ", calculée " + computedHex + ") — flash refusé",
                    true);
      // ORDRE IMPOSÉ : setOtaInProgress(false) AVANT LittleFS.begin().
      PumpController.setOtaInProgress(false);
      remountUploadFsIfNeeded();
      g_uploadOwner = nullptr;  // Chemin terminal : libérer le verrou
      return;
    }

    if (Update.end(true)) {
      systemLogger.info("Mise à jour OTA réussie. Redémarrage...");
      // Aucun verdict mémorisé sur le chemin nominal : le lambda de réponse
      // sert "OK" (corps INCHANGÉ, data/app.js en dépend) et planifie le reboot.
      if (g_restartRequested == nullptr || g_restartRequestedTime == nullptr) {
        // Pas de reboot planifié : réarmer le dosage AVANT de remonter le
        // filesystem fraîchement flashé (image saine → pas d'alerte).
        PumpController.setOtaInProgress(false);
        remountUploadFsIfNeeded(false);
      }
    } else {
      // errorString() capturé avant tout autre appel sur Update.
      const String endError = String(Update.errorString());
      failOtaUpload(request, 500,
                    String("finalisation de la mise à jour impossible (") + endError + ")");
      // ORDRE IMPOSÉ : setOtaInProgress(false) AVANT LittleFS.begin().
      PumpController.setOtaInProgress(false);
      remountUploadFsIfNeeded();
    }
    g_uploadOwner = nullptr;  // Chemin terminal (succès ou échec) : libérer le verrou
  }
}

// Allocateur ArduinoJson plafonné (bug-checkupdate-heap-tls, ADR-0032) : borne
// NETTE dure sur le JsonDocument de /check-update. Chaque bloc est préfixé d'un
// en-tête portant sa taille pour créditer le budget à la libération et au
// rétrécissement (revue 2.16.1 : ArduinoJson double ses buffers de chaîne
// pendant le parse puis les rétrécit — un comptage monotone facturerait ~4× la
// taille utile ; et un realloc de RÉTRÉCISSEMENT ne doit JAMAIS être refusé,
// StringBuilder::save() le suppose infaillible). malloc/realloc (jamais new[])
// → un refus produit nullptr, qu'ArduinoJson convertit en NoMemory — aucun
// std::bad_alloc possible sur ce chemin.
class BudgetJsonAllocator : public ArduinoJson::Allocator {
 public:
  explicit BudgetJsonAllocator(size_t capBytes) : _budget(capBytes) {}

  void* allocate(size_t size) override {
    if (!_budget.admit(size)) return nullptr;
    void* raw = malloc(size + kHeaderBytes);
    if (raw == nullptr) {
      _budget.release(size);
      return nullptr;
    }
    *static_cast<size_t*>(raw) = size;
    return static_cast<char*>(raw) + kHeaderBytes;
  }

  void deallocate(void* ptr) override {
    if (ptr == nullptr) return;
    void* raw = static_cast<char*>(ptr) - kHeaderBytes;
    _budget.release(*static_cast<size_t*>(raw));
    free(raw);
  }

  void* reallocate(void* ptr, size_t newSize) override {
    if (ptr == nullptr) return allocate(newSize);
    void* raw = static_cast<char*>(ptr) - kHeaderBytes;
    const size_t oldSize = *static_cast<size_t*>(raw);
    if (newSize > oldSize) {
      // Croissance : seul le DELTA est facturé — refus possible (NoMemory).
      if (!_budget.admit(newSize - oldSize)) return nullptr;
    } else {
      // Rétrécissement : crédit immédiat, jamais de refus budgétaire.
      _budget.release(oldSize - newSize);
    }
    void* newRaw = realloc(raw, newSize + kHeaderBytes);
    if (newRaw == nullptr) {
      // Bloc d'origine intact (sémantique realloc) : défaire l'écriture budget.
      if (newSize > oldSize) {
        _budget.release(newSize - oldSize);
      } else {
        _budget.admit(oldSize - newSize);  // re-facture ce qui vient d'être crédité
      }
      return nullptr;
    }
    *static_cast<size_t*>(newRaw) = newSize;
    return static_cast<char*>(newRaw) + kHeaderBytes;
  }

 private:
  // En-tête aligné : préserve l'alignement max du pointeur rendu à ArduinoJson.
  static constexpr size_t kHeaderBytes =
      alignof(max_align_t) > sizeof(size_t) ? alignof(max_align_t)
                                            : sizeof(size_t);
  AllocBudget _budget;
};

// Garde commune des sessions TLS sortantes GitHub (/check-update,
// /download-update) — bug-garde-tls-trop-permissive-sans-psram : mesure le
// PLUS GROS BLOC PSRAM (contiguïté, pas la PSRAM libre totale) + heap 8 bits
// RÉELLEMENT allouable, délègue la DÉCISION au prédicat pur oomTlsGateReason
// (aucune comparaison de seuil ici) et répond 503 avec le motif. Retourne true
// si la requête a été refusée : l'appelant doit sortir IMMÉDIATEMENT, sans
// aucun effet de bord.
//
// bug-165/bug-108 (v2.81.0) : les TROIS mesures ont changé de nature.
//  - psramLargestBlockBytes : heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
//    PAS ESP.getFreePsram() (total, insensible à la fragmentation — un total
//    élevé mais fragmenté n'accueillerait pas un tampon TLS de 16 Ko).
//  - freeHeapBytes/maxAllocBytes : heapFree8Bit()/heapLargestFreeBlock8Bit()
//    (ADR-0058), PAS ESP.getFreeHeap()/getMaxAllocHeap() (IRAM-inclusives —
//    c'est très exactement le défaut de bug-108 : une garde qui AUTORISAIT un
//    handshake voué à l'échec, prouvé le 15/10/2026 : ESP.getFreeHeap()=82796
//    (autorisé) pendant que maxAlloc8 réel valait 3316 o).
static bool refuseIfTlsGateClosed(AsyncWebServerRequest* request, const char* routeName,
                                  const String& clientIp, uint32_t psramLargestBlockBytes,
                                  uint32_t freeHeapBytes, uint32_t maxAllocBytes) {
  const uint8_t reason = oomTlsGateReason(psramLargestBlockBytes, freeHeapBytes, maxAllocBytes,
                                          kOtaTlsMinPsramLargestBlockBytes,
                                          kCheckUpdateMinFreeHeapBytes,
                                          kCheckUpdateMinMaxAllocBytes);
  if (reason == kOomTlsGateOk) {
    return false;
  }

  // WARN enrichi : motif + plus gros bloc PSRAM + les deux mesures heap 8 bits
  // avec leurs seuils + IP (traçabilité on-target : le refus doit être
  // auto-explicatif dans le log).
  const char* reasonLabel = (reason == kOomTlsGateNoPsram)
                                ? "plus gros bloc PSRAM absent/insuffisant"
                                : "mémoire interne 8 bits insuffisante pour TLS";
  systemLogger.warning(String(routeName) + " refusé (" + reasonLabel + ") : plus gros bloc PSRAM=" +
                       String(psramLargestBlockBytes) + " o (min " + String(kOtaTlsMinPsramLargestBlockBytes) +
                       "), heap interne 8 bits libre=" + String(freeHeapBytes) + " o (min " +
                       String(kCheckUpdateMinFreeHeapBytes) + "), plus gros bloc interne 8 bits=" +
                       String(maxAllocBytes) + " o (min " +
                       String(kCheckUpdateMinMaxAllocBytes) + "), client=" + clientIp);

  if (reason == kOomTlsGateNoPsram) {
    // Le prédicat pur regroupe sous kOomTlsGateNoPsram deux situations
    // PHYSIQUEMENT différentes ; la route les discrimine ici à partir de la
    // mesure qu'elle a déjà en main (aucun seuil dupliqué : simple test de
    // nullité) pour que le front ne mente pas à l'utilisateur.
    if (psramLargestBlockBytes == 0) {
      // PSRAM ABSENTE (module WROOM) → indisponibilité PERMANENTE, matérielle.
      // Message destiné à l'UTILISATEUR : ce n'est pas une panne mais une
      // caractéristique matérielle → formulation neutre, vocabulaire aligné sur
      // les cartes de la page Système ("Mise à jour manuelle").
      // Le champ "code" reste le contrat machine (front + tests) : ne pas le changer.
      request->send(503, "application/json",
                    "{\"error\":\"La mise à jour en ligne n'est pas disponible. Votre matériel "
                    "ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). "
                    "Utilisez la mise à jour manuelle.\",\"code\":\"psram_required\"}");
    } else {
      // PSRAM PRÉSENTE mais libre sous le seuil (WROVER momentanément saturé) →
      // indisponibilité TEMPORAIRE, réessayable : ne jamais affirmer l'absence
      // de PSRAM, et ne pas neutraliser durablement la mise à jour en ligne.
      request->send(503, "application/json",
                    "{\"error\":\"La mise à jour en ligne est momentanément indisponible : la "
                    "mémoire PSRAM est saturée. Réessayez dans quelques minutes ou utilisez la "
                    "mise à jour manuelle.\",\"code\":\"psram_low\"}");
    }
  } else {
    // Refus temporaire (heap dégradé) : code machine explicite pour que le front
    // n'ait pas à reconnaître le TEXTE (qui doit rester libre d'évoluer).
    request->send(503, "application/json",
                  "{\"error\":\"Mémoire insuffisante pour TLS, réessayez\","
                  "\"code\":\"tls_low_memory\"}");
  }
  return true;
}

// bug-165/bug-108 (v2.81.0, condition C4 du design validé security-reviewer) :
// observabilité du repli PSRAM→interne. Le correctif (mbedtls_psram_alloc.cpp)
// fait obéir mbedtls à la politique PSRAM du projet, mais la PSRAM reste un
// POOL DE TRAVAIL PARTAGÉ : si elle sature au moment d'une session TLS, les
// allocations mbedtls retombent SILENCIEUSEMENT en RAM interne — sans ce
// journal, une PSRAM saturée ramènerait l'appareil au défaut d'aujourd'hui
// sans que rien ne le signale. Appelé AVANT et APRÈS chaque session TLS
// sortante (/check-update, /download-update) : faute de pouvoir échantillonner
// PENDANT un appel HTTPClient bloquant sans ré-instrumenter mbedtls (cf.
// l'instrumentation complète, non embarquée, conservée en
// docs/incidents/bug-165-instrumentation-tls.patch), le relevé « après »
// (pris immédiatement après le retour de https.GET()/http.GET(), donc le plus
// près possible de la fin du handshake) sert de proxy pour « pendant ».
static void logTlsHeapWindow(const char* routeName, const char* whenLabel) {
  const uint32_t internalLargest = heapLargestFreeBlock8Bit();
  const uint32_t internalFree = heapFree8Bit();
  const uint32_t psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  systemLogger.info(String(routeName) + " : fenêtre TLS (" + whenLabel +
                    ") — interne 8 bits libre=" + String(internalFree) +
                    " o, plus gros bloc interne=" + String(internalLargest) +
                    " o, plus gros bloc PSRAM=" + String(psramLargest) + " o");
  if (internalLargest < kOtaTlsInternalFallbackWarnBytes) {
    systemLogger.warning(String(routeName) + " : plus gros bloc interne 8 bits (" +
                         whenLabel + ") sous " + String(kOtaTlsInternalFallbackWarnBytes) +
                         " o (" + String(internalLargest) +
                         " o) — repli PSRAM→interne probable pendant la session TLS "
                         "(PSRAM peut-être saturée)");
  }
}

static void handleCheckUpdate(AsyncWebServerRequest* request) {
  // bug-oom-accept-http-durcissement : IP client dans les logs (le crash du
  // 11/08 avait un profil TLS compatible mais l'appelant était invérifiable).
  String clientIp = request->client()->remoteIP().toString();
  // bug-165/bug-108 (v2.81.0) : PLUS GROS BLOC PSRAM (contiguïté — un total
  // élevé mais fragmenté n'accueillerait pas un tampon TLS de 16 Ko) et heap
  // 8 bits RÉELLEMENT allouable (ADR-0058, jamais ESP.getFreeHeap()/
  // getMaxAllocHeap(), IRAM-inclusives).
  uint32_t psramLargestBlockBytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  uint32_t freeHeapBytes = heapFree8Bit();
  uint32_t maxAllocBytes = heapLargestFreeBlock8Bit();

  // Garde PSRAM + heap 8 bits AVANT la création de WiFiClientSecure : sans PSRAM
  // le handshake est voué à l'échec après avoir affamé le tas (~1 s de fenêtre
  // de crash) ; sur heap dégradé l'échec était lent et aléatoire (BIGNUM alloc
  // en cours de handshake). Refus 503 rapide et honnête à la place.
  if (refuseIfTlsGateClosed(request, "/check-update", clientIp, psramLargestBlockBytes,
                            freeHeapBytes, maxAllocBytes)) {
    return;
  }

  // bug-checkupdate-tls-core3 : l'échec observé est un manque de mémoire
  // pendant le handshake (X509 alloc failed) — tracer l'état heap AVANT la
  // session TLS pour distinguer épuisement total / fragmentation (maxAlloc).
  // Plus gros bloc PSRAM tracé aussi : il conditionne désormais l'autorisation.
  systemLogger.info("Vérification des mises à jour GitHub... (plus gros bloc PSRAM=" +
                    String(psramLargestBlockBytes) + " o, heap interne 8 bits libre=" +
                    String(freeHeapBytes) + " o, plus gros bloc interne 8 bits=" +
                    String(maxAllocBytes) + " o, client=" + clientIp + ")");
  // Condition C4 (bug-165/bug-108) : repère AVANT la session, pour confronter
  // à l'APRÈS et détecter un repli PSRAM→interne survenu PENDANT le handshake.
  logTlsHeapWindow("/check-update", "avant");

  WiFiClientSecure client;
  if (!ensureTimeForTls(request)) {
    return;
  }
  client.setCACert(GITHUB_ROOT_CA);

  HTTPClient https;

  // Utiliser l'API GitHub pour récupérer la dernière release
  const char* apiUrl = "https://api.github.com/repos/niko34/esp32-pool-controller/releases/latest";

  if (!https.begin(client, apiUrl)) {
    systemLogger.error("Impossible de se connecter à GitHub");
    sendErrorResponse(request, 500, "Connection failed");
    return;
  }

  https.addHeader("User-Agent", "ESP32-Pool-Controller");
  // bug-checkupdate-heap-tls (ADR-0032) : HTTP/1.0 interdit le Transfer-Encoding
  // chunked (patron ArduinoJson officiel) → getStream() livre le corps brut,
  // parsable en flux sans le stocker entier.
  https.useHTTP10(true);

  int httpCode = https.GET();
  // Condition C4 : repère APRÈS le handshake (succès ou échec) — le point le
  // plus proche de « pendant » qu'un appel HTTPClient bloquant permette de
  // mesurer sans ré-instrumenter mbedtls.
  logTlsHeapWindow("/check-update", "session ouverte");

  if (httpCode != HTTP_CODE_OK) {
    String errorDetail = "";
    char tlsErr[128] = {0};
    int tlsCode = 0;
    if (httpCode < 0) {
      // bug-checkupdate-tls-core3 : un code négatif = échec côté client
      // (TLS/socket) — remonter le détail mbedtls au lieu d'un -1 muet.
      tlsCode = client.lastError(tlsErr, sizeof(tlsErr));
      if (tlsCode != 0) {
        errorDetail = String(" — TLS: ") + tlsErr + " (" + String(tlsCode) + ")";
      }
      errorDetail += String(" [") + HTTPClient::errorToString(httpCode) + "]";
    }
    systemLogger.error("Erreur HTTP GitHub: " + String(httpCode) + errorDetail);
    https.end();

    // Cas spécial : 404 signifie qu'aucune release n'existe
    if (httpCode == 404) {
      JsonDocument response;
      response["current_version"] = FIRMWARE_VERSION;
      response["latest_version"] = FIRMWARE_VERSION;
      response["update_available"] = false;
      response["no_release"] = true;
      response["message"] = "Aucune release disponible sur GitHub";

      String json;
      serializeJson(response, json);

      systemLogger.info("Aucune release GitHub trouvée");
      request->send(200, "application/json", json);
      return;
    }

    // Autres erreurs HTTP — bug-165 : l'échec était MUET côté réponse HTTP
    // (`code: -1` opaque) alors que le journal avait déjà le détail mbedtls.
    // classifyOtaCheckFailure() NOMME l'étape ; le détail mbedtls (message
    // générique de bibliothèque, aucune IP/chemin/secret) est repris tel quel
    // — c'est la même chaîne que celle déjà journalisée ligne au-dessus.
    JsonDocument response;
    response["error"] = "GitHub API error";
    response["code"] = httpCode;
    switch (classifyOtaCheckFailure(httpCode, tlsCode)) {
      case OtaCheckFailureStep::kTlsHandshake:
        response["step"] = "tls_handshake";
        response["detail"] = String(tlsErr) + " (" + String(tlsCode) + ")";
        break;
      case OtaCheckFailureStep::kConnection:
        response["step"] = "connection";
        response["detail"] = "Connexion impossible (résolution de nom ou TCP refusé)";
        break;
      case OtaCheckFailureStep::kHttpOther:
        response["step"] = "http";
        response["detail"] = HTTPClient::errorToString(httpCode);
        break;
    }

    String errorMsg;
    serializeJson(response, errorMsg);
    request->send(500, "application/json", errorMsg);
    return;
  }

  // bug-173 (ADR-0032 révisé par ADR-0112) : la réponse GitHub n'est PLUS
  // parsée EN FLUX. Le lecteur de flux d'ArduinoJson traite un read() qui rend
  // -1 (tampon réseau TLS transitoirement vide, PAS fin de document) comme une
  // fin de fichier — prouvé en production le 2026-10-18 (55 ms entre
  // l'ouverture TLS et "IncompleteInput", bien trop court pour avoir lu
  // 16,7 Ko). Elle est désormais mémorisée dans un tampon PSRAM BORNÉ
  // (kOtaCheckResponseMaxBytes), puis parsée DEPUIS CE TAMPON — filtre
  // ArduinoJson conservé à l'identique, seuls les champs listés ci-dessous
  // entrent dans le document.
  //
  // ADR-0032 avait délibérément écarté le chargement intégral en RAM
  // (getString() culminait à ~35 Ko de RAM INTERNE, en plus des ~48 Ko de la
  // session TLS → échec systématique sur tas fragmenté). Ce n'était PAS une
  // erreur : c'est le CONTEXTE qui a changé — depuis ADR-0112, mbedtls alloue
  // en PSRAM (~4,1 Mo libres mesurés le même jour), et le tampon ci-dessous
  // est alloué EXPLICITEMENT en PSRAM (MALLOC_CAP_SPIRAM, aucun repli
  // implicite vers l'interne), placement VÉRIFIÉ par esp_ptr_external_ram()
  // plutôt que supposé (le projet a déjà eu un parachute « en PSRAM » resté
  // décoratif faute de vérification).
  int declaredContentLength = https.getSize();  // entrée TIERCE : indicatif, jamais de confiance aveugle

  // ⚠️ TAILLE AJUSTÉE À LA RÉPONSE ANNONCÉE (relevé en revue) : allouer 128 Ko
  // à chaque appel alors que la réponse en fait ~17 divise par ~8 la PSRAM
  // contiguë exigée, et rend d'autant plus rare le refus transitoire ci-dessous.
  // ⛔ `declaredContentLength` reste une entrée TIERCE : il ne sert qu'à
  // DIMENSIONNER, jamais à valider. Un pair qui annoncerait moins que ce qu'il
  // envoie est REFUSÉ — ⚠️ mais pas toujours par la même cause (relevé en
  // revue de sécurité) : selon le découpage des paquets, soit une lecture
  // atteint la capacité et rend « trop gros », soit l'arrêt anticipé sur
  // `declared` tombe d'abord et le parse échoue ensuite en `IncompleteInput`.
  // ⇒ Les deux refusent — fail-closed tenu — ⛔ mais ne pas promettre ici un
  // verdict déterministe qui n'en est pas un.
  // ⚠️ `+1` : la capacité UTILE du prédicat d'arrêt est `capacity - 1` (il
  // refuse dès `bytesRead >= capacity`), donc une réponse de exactement
  // `declaredContentLength` octets doit pouvoir entrer.
  // Absent ou nul (⛔ `getSize()` rend -1 quand l'en-tête manque) ⇒ borne pleine.
  const size_t bufferCapacity =
      (declaredContentLength > 0 &&
       static_cast<size_t>(declaredContentLength) + 1 < kOtaCheckResponseMaxBytes)
          ? static_cast<size_t>(declaredContentLength) + 1
          : kOtaCheckResponseMaxBytes;

  uint8_t* rawBuffer = static_cast<uint8_t*>(
      heap_caps_malloc(bufferCapacity, MALLOC_CAP_SPIRAM));
  if (rawBuffer == nullptr) {
    // Refus EXPLICITE : jamais de repli silencieux vers la RAM interne — ce
    // serait exactement le défaut qu'ADR-0032 avait corrigé.
    // 🔴 CODE `psram_low`, ⛔ SURTOUT PAS `psram_required` (relevé en revue).
    // ⚠️ `psram_required` est un CONTRAT MACHINE qui signifie « ce matériel n'a
    // PAS de PSRAM » : docs/API.md impose au client de le traiter comme
    // DÉFINITIF, et data/app.js grise alors « Vérifier » ET « Installer » sans
    // jamais les réarmer. Or ICI la PSRAM existe — c'est un bloc contigu de
    // 128 Ko qui manque à cet instant, condition TRANSITOIRE (fragmentation,
    // session TLS en PSRAM depuis ADR-0112).
    // ⇒ L'émettre ici afficherait « matériel incompatible » sur un WROVER sain
    // et sortirait l'utilisateur de la mise à jour en ligne jusqu'au
    // rechargement de la page. `psram_low` est déjà la sémantique « saturée,
    // réessayez » (toast + loadSystemInfo(), aucun verrouillage).
    // ⛔ Et le texte ⛔ n'affirme RIEN sur le matériel.
    systemLogger.error("/check-update : bloc PSRAM contigu indisponible pour le tampon de réponse (" +
                       String(bufferCapacity) + " o demandés) — condition transitoire");
    https.end();
    request->send(503, "application/json",
                  "{\"error\":\"Mémoire PSRAM momentanément insuffisante pour lire la réponse "
                  "GitHub — réessayez dans un instant\","
                  "\"code\":\"psram_low\"}");
    return;
  }
  if (!esp_ptr_external_ram(rawBuffer)) {
    // Témoin anti-décoratif (même idiome que mbedtls_psram_alloc.cpp) :
    // MALLOC_CAP_SPIRAM ne devrait JAMAIS atterrir ailleurs qu'en PSRAM — si
    // ça arrivait quand même, refus plutôt que de parser depuis un tampon dont
    // la localisation réelle n'a pas été confirmée.
    systemLogger.critical("/check-update : tampon de réponse alloué hors PSRAM malgré "
                          "MALLOC_CAP_SPIRAM — refus (jamais de parse non vérifié)");
    heap_caps_free(rawBuffer);
    https.end();
    sendErrorResponse(request, 500, "Internal buffer placement error");
    return;
  }

  // Lecture BORNÉE : boucle tant que la connexion est exploitable (ouverte OU
  // des octets restent immédiatement disponibles) ET que l'échéance totale
  // n'est pas atteinte — décision déléguée au prédicat pur
  // otaBufferedReadNextAction (ota_response_buffer_logic.h), testé nativement
  // par mutation. `dataAvailable` est TOUJOURS revérifié : un read() qui rend
  // 0 octet immédiatement disponible ne signifie PAS "fin du document", c'est
  // très exactement le défaut corrigé par ce bug.
  WiFiClient& rawStream = https.getStream();
  size_t bytesRead = 0;
  const unsigned long readStartMs = millis();
  size_t readIterations = 0;
  OtaBufferedReadAction stopAction = OtaBufferedReadAction::kContinue;
  for (;;) {
    // ⚠️ available() lu UNE SEULE fois par itération (relevé en revue) : deux
    // lectures pouvaient rendre >0 puis 0, donnant toRead == 0, readBytes() == 0
    // et un tour de boucle SANS vTaskDelay ⇒ boucle chaude sur async_tcp
    // jusqu'à l'échéance.
    const int availableNow = rawStream.available();
    const bool dataAvailable = availableNow > 0;
    const bool deadlineExceeded =
        (millis() - readStartMs) >= kOtaCheckResponseReadTimeoutMs;  // immunisé au repliement de millis()
    stopAction = otaBufferedReadNextAction(bytesRead, bufferCapacity,
                                          https.connected(), dataAvailable, deadlineExceeded);
    if (stopAction != OtaBufferedReadAction::kContinue) break;

    // ⚠️ ARRÊT NORMAL SUR content-length (relevé en revue) : sans lui, la seule
    // sortie du chemin nominal est la fermeture par le pair. `useHTTP10(true)`
    // envoie bien `Connection: close`, donc ça se termine AUJOURD'HUI — mais un
    // intermédiaire qui n'honore pas la fermeture ferait tourner la boucle
    // jusqu'aux 15 s, bloquerait la tâche async_tcp pendant ce temps, puis
    // rendrait un verdict « trop lent » FAUX alors que le document était
    // complet. ⛔ `declaredContentLength` reste une entrée TIERCE : on ne s'en
    // sert que pour s'arrêter PLUS TÔT, jamais pour valider le contenu (le
    // contrôle de cohérence a lieu après la boucle).
    if (declaredContentLength > 0 &&
        bytesRead >= static_cast<size_t>(declaredContentLength)) {
      stopAction = OtaBufferedReadAction::kStopOk;
      break;
    }

    if (dataAvailable) {
      const size_t toRead = min(static_cast<size_t>(availableNow),
                                bufferCapacity - bytesRead);
      const int n = rawStream.readBytes(reinterpret_cast<char*>(rawBuffer + bytesRead), toRead);
      if (n > 0) bytesRead += static_cast<size_t>(n);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    // ⚠️ Watchdog nourri une itération sur 16 (⛔ PAS à chaque tour — la
    // formulation d'origine était fausse, relevé en revue).
    // 🔴 ET IL PROTÈGE RÉELLEMENT : la tâche AsyncTCP **EST** abonnée au TWDT.
    // Vérifié dans la bibliothèque, ⛔ pas supposé : `CONFIG_ASYNC_TCP_USE_WDT`
    // vaut **1** par défaut (AsyncTCP.h:41-42), le `#undef` d'AsyncTCP.cpp:18
    // est à l'intérieur d'un bloc `LIBRETINY` (autre plateforme), et
    // `platformio.ini` ne surcharge rien ⇒ `esp_task_wdt_add(NULL)` à
    // AsyncTCP.cpp:322 s'exécute bel et bien.
    // ⚠️ Deux revues se sont contredites sur ce point ; l'une s'appuyait sur un
    // commentaire de CE fichier qui est faux (corrigé, voir plus bas). ⛔ Sans
    // ce nourrissage, une lecture lente de 15 s dépasserait les 30 s du TWDT
    // en cumulé avec le handshake.
    if ((++readIterations % 16) == 0) {
      esp_task_wdt_reset();
    }
  }
  https.end();

  const bool lengthMismatch = otaBufferedReadLengthMismatch(bytesRead, declaredContentLength);
  const OtaBufferedReadResult readResult = otaBufferedReadResultFor(stopAction, lengthMismatch);

  if (readResult != OtaBufferedReadResult::kOk) {
    heap_caps_free(rawBuffer);
    // Distinction EXPLICITE dans le journal (bug-173) : un refus muet ou
    // ambigu est le travers récurrent de ce projet.
    switch (readResult) {
      case OtaBufferedReadResult::kTooLarge:
        systemLogger.error("/check-update : réponse GitHub trop grosse (> " +
                           String(kOtaCheckResponseMaxBytes) + " o) — refusée");
        sendErrorResponse(request, 500, "GitHub response exceeds read buffer");
        break;
      case OtaBufferedReadResult::kTimedOut:
        systemLogger.error("/check-update : lecture de la réponse GitHub trop lente (> " +
                           String(kOtaCheckResponseReadTimeoutMs) + " ms, lu " +
                           String(bytesRead) + " o) — abandonnée");
        sendErrorResponse(request, 500, "GitHub response read timeout");
        break;
      case OtaBufferedReadResult::kIncomplete:
      default:
        systemLogger.error("/check-update : réponse GitHub incomplète (lu " + String(bytesRead) +
                           " / annoncé " + String(declaredContentLength) +
                           " o) — pair coupé ou flux tronqué");
        sendErrorResponse(request, 500, "GitHub response incomplete");
        break;
    }
    return;
  }

  systemLogger.info("/check-update : réponse GitHub lue intégralement (" + String(bytesRead) + " o, PSRAM)");

  // Filtre ArduinoJson CONSERVÉ à l'identique (bug-173) : seuls les champs
  // réellement consommés entrent dans le document.
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["name"] = true;
  filter["published_at"] = true;
  filter["body"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["digest"] = true;

  BudgetJsonAllocator budgetAlloc(kOtaCheckJsonBudgetBytes);
  JsonDocument doc(&budgetAlloc);
  // `rawBuffer` est passé en `const char*` : ArduinoJson copie les chaînes
  // dans le document (StringBuilder) plutôt que de référencer le tampon en
  // place (ce zero-copy n'existe que pour un `char*` MUTABLE) — le tampon peut
  // donc être libéré juste après, sans pointeur pendant dans `doc`.
  DeserializationError error = deserializeJson(
      doc, reinterpret_cast<const char*>(rawBuffer), bytesRead,
      DeserializationOption::Filter(filter));
  heap_caps_free(rawBuffer);

  if (error) {
    if (error == DeserializationError::NoMemory) {
      // Budget kOtaCheckJsonBudgetBytes dépassé (corps de release anormalement
      // gros) ou heap réellement épuisé : refus propre, pas de crash.
      systemLogger.error("Réponse GitHub hors budget JSON /check-update (" +
                         String(kOtaCheckJsonBudgetBytes) + " o)");
      sendErrorResponse(request, 500, "GitHub response too large");
    } else {
      systemLogger.error("Erreur parsing JSON GitHub: " + String(error.c_str()));
      sendErrorResponse(request, 500, "JSON parse error");
    }
    return;
  }

  String latestVersion = doc["tag_name"].as<String>();
  String currentVersion = FIRMWARE_VERSION;

  // Retirer le 'v' si présent au début de la version GitHub
  if (latestVersion.startsWith("v") || latestVersion.startsWith("V")) {
    latestVersion = latestVersion.substring(1);
  }

  bool updateAvailable = (latestVersion != currentVersion);

  // Chercher les assets (firmware.bin et littlefs.bin)
  String firmwareUrl = "";
  String filesystemUrl = "";
  // Empreintes SHA-256 des assets (champ `digest` de l'API GitHub, format
  // "sha256:<64hex>"). Chaîne vide si absent — le refus fail-closed se fait
  // au moment du téléchargement (/download-update), pas ici.
  String firmwareDigest = "";
  String filesystemDigest = "";
  // feature-151 D1 : URL des fichiers de signature `.sig`, SI présents parmi
  // les assets de la release — champs ADDITIFS (chaîne vide si absents,
  // aucune rupture de contrat pour un client qui les ignore). Relayés pour
  // que /download-update ait un jour quelque chose à vérifier (D1 ne les
  // exploite pas encore côté page web, hors périmètre de ce lot).
  String firmwareSigUrl = "";
  String filesystemSigUrl = "";

  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = asset["name"].as<String>();
    if (name == "firmware.bin") {
      firmwareUrl = asset["browser_download_url"].as<String>();
      firmwareDigest = asset["digest"] | "";
    } else if (name == "littlefs.bin") {
      filesystemUrl = asset["browser_download_url"].as<String>();
      filesystemDigest = asset["digest"] | "";
    } else if (name == "firmware.bin.sig") {
      firmwareSigUrl = asset["browser_download_url"].as<String>();
    } else if (name == "littlefs.bin.sig") {
      filesystemSigUrl = asset["browser_download_url"].as<String>();
    }
  }

  // Construire la réponse
  JsonDocument response;
  response["current_version"] = currentVersion;
  response["latest_version"] = latestVersion;
  response["update_available"] = updateAvailable;
  response["firmware_url"] = firmwareUrl;
  response["filesystem_url"] = filesystemUrl;
  // Champs additifs (feature-026) : relayés par l'UI vers /download-update
  response["firmware_digest"] = firmwareDigest;
  response["filesystem_digest"] = filesystemDigest;
  // Champs additifs (feature-151 D1) : URL des fichiers `.sig`, vides si la
  // release n'en contient pas (releases publiées avant ce lot).
  response["firmware_sig_url"] = firmwareSigUrl;
  response["filesystem_sig_url"] = filesystemSigUrl;
  response["release_name"] = doc["name"];
  response["published_at"] = doc["published_at"];
  // bug-crash-getlogs-badalloc (audit) : le corps de release GitHub n'est pas
  // borné → tronquer pour borner la réponse construite en RAM.
  // bug-checkupdate-heap-tls : coupe sur une frontière UTF-8 (les notes en
  // français contiennent accents/emojis — couper un caractère multi-octets en
  // deux produisait un JSON invalide côté navigateur).
  String releaseNotes = doc["body"].as<String>();
  if (releaseNotes.length() > kOtaReleaseNotesMaxChars) {
    size_t cut = utf8SafeCutLen(releaseNotes.c_str(), releaseNotes.length(),
                                kOtaReleaseNotesMaxChars);
    releaseNotes = releaseNotes.substring(0, cut) + "…";
  }
  response["release_notes"] = releaseNotes;

  String json;
  serializeJson(response, json);

  systemLogger.info("Version actuelle: " + currentVersion + ", Dernière version: " + latestVersion);
  request->send(200, "application/json", json);
}

// Envoie la réponse 422 structurée en cas d'échec d'intégrité (feature-026).
static void sendIntegrityMismatch(AsyncWebServerRequest* request, const char* expectedHex,
                                  const char* computedHex, const char* message) {
  JsonDocument err;
  err["error"] = "integrity_mismatch";
  err["expected"] = expectedHex;
  err["computed"] = computedHex;
  err["message"] = message;
  String json;
  serializeJson(err, json);
  request->send(422, "application/json", json);
}

static void handleDownloadUpdate(AsyncWebServerRequest* request) {
  // Récupérer l'URL du fichier à télécharger
  if (!request->hasParam("url", true)) {
    sendErrorResponse(request, 400, "Missing URL parameter");
    return;
  }

  String url = request->getParam("url", true)->value();

  // SÉCURITÉ: Valider que l'URL provient d'un hôte autorisé
  if (!isUrlAllowed(url)) {
    systemLogger.warning("Tentative de téléchargement OTA depuis un hôte non autorisé: " + url);
    sendErrorResponse(request, 403, "URL not allowed (host not whitelisted)");
    return;
  }

  // SÉCURITÉ (feature-026) : empreinte SHA-256 REQUISE — fail-closed strict.
  // Le digest provient du champ `digest` des assets GitHub (relayé par l'UI
  // depuis /check-update). Absent ou malformé → refus AVANT tout téléchargement.
  if (!request->hasParam("digest", true)) {
    systemLogger.critical("Intégrité OTA: téléchargement refusé — empreinte SHA-256 absente (fail-closed)");
    request->send(400, "application/json",
      "{\"error\":\"integrity_digest_missing\",\"message\":\"Empreinte SHA-256 absente : téléchargement refusé (vérification d'intégrité obligatoire)\"}");
    return;
  }

  String digestStr = request->getParam("digest", true)->value();
  uint8_t expectedHash[32];
  if (!parseSha256Digest(digestStr.c_str(), expectedHash)) {
    systemLogger.critical("Intégrité OTA: téléchargement refusé — empreinte SHA-256 invalide: " + digestStr);
    request->send(400, "application/json",
      "{\"error\":\"integrity_digest_invalid\",\"message\":\"Empreinte SHA-256 invalide : téléchargement refusé (format attendu sha256:<64 hex>)\"}");
    return;
  }

  // Déterminer le type de mise à jour en fonction de l'URL — hissé ICI
  // (feature-151 D1, avant tout le reste) : la vérification d'authenticité
  // ci-dessous a besoin du genre ATTENDU pour détecter un éventuel « genre
  // discordant », et doit elle-même précéder la garde PSRAM/heap et tout
  // effet de bord (condition 2 du verdict security-reviewer P1).
  bool isFilesystem = (url.indexOf("littlefs") >= 0 || url.indexOf("filesystem") >= 0);

  // feature-151 D1, lot B1 : hex DÉCLARÉ par un manifeste PARSÉ, portée locale
  // à CETTE requête (handleDownloadUpdate est synchrone, pas de chunks à
  // traverser — pas besoin de statique ici, à la différence de
  // g_uploadManifestSha256Hex côté /update). PAS pré-décodé en binaire (voir
  // le commentaire de g_uploadManifestSha256Hex, web_routes_ota.cpp) : le
  // décodage vit dans ota_manifest_binding_logic.cpp. Alimentée dans le bloc
  // D1 ci-dessous, CONSOMMÉE plus bas (comparaison 2/2, une fois le flux
  // téléchargé et son empreinte RÉELLEMENT calculée).
  char manifestSha256Hex[65];
  bool manifestDigestValid = false;

  // ---- feature-151 D1 : vérification d'AUTHENTICITÉ — JOURNALISE SEULEMENT --
  // AVANT refuseIfTlsGateClosed (Q0/condition 1 du verdict security-reviewer
  // P1) et avant tout esp_partition_erase_range()/LittleFS.end() : place
  // DÉFINITIVE pour D3, mais ⛔ AUCUN `return` de refus ici — D1 ne fait que
  // calculer et journaliser le verdict que D3 appliquera. Mêmes paramètres
  // `manifest`/`signature` que /update ; absents aujourd'hui sur ce chemin
  // (la page web ne les envoie pas encore, hors périmètre de ce lot) →
  // verdict kAbsent, journalisé en INFO, sans conséquence.
  // GARDE STRUCTURELLE : test/test_native_ota_order_guard (guard [G6]) fige
  // cette place — ne pas la déplacer sans mettre à jour ce test.
  {
    const OtaManifestKind expectedKind =
        isFilesystem ? OtaManifestKind::kFilesystem : OtaManifestKind::kFirmware;
    const bool manifestPresent = request->hasParam("manifest", true);
    const bool signaturePresent = request->hasParam("signature", true);
    const String manifestRaw =
        manifestPresent ? request->getParam("manifest", true)->value() : String();
    const String signatureRaw =
        signaturePresent ? request->getParam("signature", true)->value() : String();
    const OtaAuthenticityComputation authenticity = computeOtaAuthenticityVerdict(
        manifestPresent, signaturePresent, manifestRaw.c_str(), signatureRaw.c_str(),
        expectedKind);
    logOtaAuthenticityVerdict("/download-update", authenticity);

    // ---- lot B1 : liaison manifeste ↔ image — comparaison 1/2 ---------------
    // Au paramètre `digest` DÉJÀ décodé ci-dessus (`expectedHash`) : contrairement
    // à /update, ce décodage a déjà eu lieu AVANT ce bloc (la route exige le
    // digest en fail-closed dès l'entrée) — aucune relecture de requête nécessaire.
    manifestDigestValid = authenticity.fieldsValid;
    if (authenticity.fieldsValid) {
      memcpy(manifestSha256Hex, authenticity.fields.sha256Hex, sizeof(manifestSha256Hex));
    }
    logOtaManifestBindingVerdict(
        "/download-update", "paramètre digest (avant téléchargement)",
        otaManifestBindingVerdict(manifestDigestValid, manifestSha256Hex,
                                  /*comparedDigestValid=*/true, expectedHash));
  }

  // Garde PSRAM + heap 8 bits avant la session TLS sortante
  // (bug-garde-tls-trop-permissive-sans-psram, bug-165/bug-108) : MÊME garde
  // que /check-update. Position volontaire — après url/allowlist/digest/
  // authenticité (donc une requête mal formée reste un 400/403), mais AVANT
  // WiFiClientSecure, setOtaInProgress(), LittleFS.end() et tout
  // esp_partition_erase_range() : un refus n'a AUCUN effet de bord (pas de
  // pompes gelées, pas de FS démonté, rien d'effacé).
  {
    String clientIp = request->client()->remoteIP().toString();
    // bug-165/bug-108 : plus gros bloc PSRAM (contiguïté) et heap 8 bits
    // RÉELLEMENT allouable — cf. /check-update, mêmes motifs.
    if (refuseIfTlsGateClosed(request, "/download-update", clientIp,
                              heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                              heapFree8Bit(), heapLargestFreeBlock8Bit())) {
      return;
    }
    // Condition C4 : repère AVANT la session.
    logTlsHeapWindow("/download-update", "avant");
  }

  // Paramètre optionnel pour contrôler le redémarrage
  bool shouldRestart = true;
  if (request->hasParam("restart", true)) {
    String restartParam = request->getParam("restart", true)->value();
    shouldRestart = (restartParam == "true" || restartParam == "1");
  }

  if (isFilesystem) {
    systemLogger.info("Téléchargement mise à jour filesystem depuis GitHub");
  } else {
    systemLogger.info("Téléchargement mise à jour firmware depuis GitHub");
  }

  // Créer un client HTTPS
  WiFiClientSecure client;
  if (!ensureTimeForTls(request)) {
    return;
  }

  // Validation TLS complète avec certificats couvrant github.com ET objects.githubusercontent.com
  // Le fichier github_root_ca.h contient les chaînes RSA et ECC pour tous les domaines GitHub
  client.setCACert(GITHUB_ROOT_CA);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    systemLogger.error("Impossible de se connecter à GitHub pour téléchargement");
    sendErrorResponse(request, 500, "Connection failed");
    return;
  }

  int httpCode = http.GET();
  // Condition C4 : repère APRÈS le handshake (succès ou échec) — cf. /check-update.
  logTlsHeapWindow("/download-update", "session ouverte");

  if (httpCode != HTTP_CODE_OK) {
    systemLogger.error("Erreur HTTP téléchargement: " + String(httpCode));
    http.end();
    sendErrorResponse(request, 500, "Download failed");
    return;
  }

  int contentLength = http.getSize();

  if (contentLength <= 0) {
    systemLogger.error("Taille fichier invalide");
    http.end();
    sendErrorResponse(request, 500, "Invalid file size");
    return;
  }

  systemLogger.info("Taille du fichier: " + String(contentLength) + " octets");

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  // ⚠️ INVARIANT (bug-165/bug-108, v2.81.0) : aucun tampon détenu par mbedtls
  // ne doit être passé à Update.write() / esp_partition_write() (équivalent
  // esp_ota_write pour la voie firmware). La copie par ce tampon de PILE
  // (buff[512]) est STRUCTURELLE, pas incidente : stream->readBytes() copie le
  // contenu hors de l'espace mbedtls avant toute écriture flash. C'est ce qui
  // rend sûre la bascule PSRAM des buffers mbedtls ci-dessus — pendant
  // l'écriture en flash, le cache qui donne accès à la PSRAM est désactivé ;
  // si mbedtls y détenait des données à cet instant, l'appareil planterait.
  // Ce commentaire ne change AUCUNE logique de la boucle ci-dessous.
  uint8_t buff[512];

  // Hachage incrémental du flux téléchargé (feature-026) — instance LOCALE
  // au handler, comparée à l'empreinte GitHub avant validation du flash.
  OtaStreamHasher hasher;
  hasher.begin();

  if (isFilesystem) {
    // Mise à jour du filesystem: écriture directe dans la partition SPIFFS
    const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);

    if (!partition) {
      systemLogger.error("Partition SPIFFS non trouvée");
      http.end();
      sendErrorResponse(request, 500, "SPIFFS partition not found");
      return;
    }

    if (contentLength > partition->size) {
      systemLogger.error("Image trop grande pour la partition");
      http.end();
      sendErrorResponse(request, 500, "Image too large for partition");
      return;
    }

    PumpController.setOtaInProgress(true);

    // Démonter LittleFS avant l'écriture
    LittleFS.end();
    systemLogger.info("LittleFS démonté pour mise à jour");

    // Effacer la partition
    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
      systemLogger.error("Erreur effacement partition: " + String(esp_err_to_name(err)));
      http.end();
      PumpController.setOtaInProgress(false);
      sendErrorResponse(request, 500, "Partition erase failed");
      return;
    }
    systemLogger.info("Partition effacée, début de l'écriture...");

    // Lire et écrire les données par blocs
    size_t yieldCounter = 0;
    while (http.connected() && (written < (size_t)contentLength)) {
      size_t available = stream->available();

      if (available) {
        int c = stream->readBytes(buff, min(available, sizeof(buff)));

        if (c > 0) {
          hasher.update(buff, c);  // Hachage incrémental du flux (feature-026)
          err = esp_partition_write(partition, written, buff, c);
          if (err != ESP_OK) {
            systemLogger.error("Erreur écriture partition: " + String(esp_err_to_name(err)));
            http.end();
            PumpController.setOtaInProgress(false);
            sendErrorResponse(request, 500, "Partition write failed");
            return;
          }
          written += c;
          yieldCounter++;

          // Yield et feed watchdog toutes les 8 écritures (~4KB)
          if (yieldCounter % 8 == 0) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(10));
          }

          // Log de progression tous les 100KB
          if (written % 102400 == 0 || written == (size_t)contentLength) {
            unsigned int percent = (written * 100) / contentLength;
            systemLogger.info("Téléchargement FS: " + String(percent) + "%");
            esp_task_wdt_reset();
          }
        }
      }
      delay(1);
    }

    http.end();

    // Vérification d'intégrité (feature-026) : couvre aussi le téléchargement
    // tronqué (coupure WiFi) — l'empreinte d'un flux incomplet ne correspondra pas.
    uint8_t computedFs[32];
    hasher.finish(computedFs);

    // feature-151 D1, lot B1 : comparaison 2/2 — l'empreinte DÉCLARÉE par le
    // manifeste (si authentifiable) contre l'empreinte RÉELLEMENT téléchargée
    // et écrite en partition. ⛔ JOURNALISE SEULEMENT.
    logOtaManifestBindingVerdict(
        "/download-update", "empreinte calculée (fichier filesystem reçu)",
        otaManifestBindingVerdict(manifestDigestValid, manifestSha256Hex,
                                  /*comparedDigestValid=*/true, computedFs));

    if (!sha256Equal(computedFs, expectedHash)) {
      char expectedHex[kOtaSha256HexLen + 1];
      char computedHex[kOtaSha256HexLen + 1];
      sha256ToHex(expectedHash, expectedHex, sizeof(expectedHex));
      sha256ToHex(computedFs, computedHex, sizeof(computedHex));
      systemLogger.critical("Intégrité OTA FS: empreinte SHA-256 non conforme (attendue " +
                            String(expectedHex) + ", calculée " + String(computedHex) +
                            ") — redémarrage annulé");
      // ORDRE IMPOSÉ (gate pool-chemistry 2.20.1) : réarmer le dosage AVANT le
      // montage — ⛔ ORDRE INCHANGÉ, il reste juste.
      // 🔴 MAIS SA JUSTIFICATION ÉTAIT FAUSSE (relevé en revue de sécurité,
      // 2026-10-18, vérifié dans la bibliothèque) : elle affirmait que « la
      // tâche AsyncTCP n'est PAS abonnée au watchdog ». ⚠️ Elle L'EST :
      // `CONFIG_ASYNC_TCP_USE_WDT` vaut 1 par défaut (AsyncTCP.h:41-42), le
      // `#undef` d'AsyncTCP.cpp:18 ne s'applique qu'à LIBRETINY, et
      // `platformio.ini` ne surcharge rien.
      // ⇒ Un montage long ou un panic PROVOQUE donc bien un reboot. L'ordre
      // reste néanmoins le bon : le réarmement est un drapeau RAM, et le faire
      // avant garantit l'état sûr **sans dépendre** du reboot pour y arriver.
      // ⚠️ À faire reconfirmer par `pool-chemistry` : sa décision reposait sur
      // une prémisse inexacte, même si la conclusion tient. ⛔ Ne pas changer
      // cet ordre sans son verdict.
      PumpController.setOtaInProgress(false);  // Appariement du setOtaInProgress(true)
      // Remontage best-effort : la partition contient une image corrompue,
      // LittleFS échouera probablement (l'UI restera servie depuis le cache
      // navigateur jusqu'à un nouvel OTA FS réussi).
      if (!LittleFS.begin(false)) {
        systemLogger.warning("Impossible de remonter LittleFS après échec d'intégrité");
      }
      sendIntegrityMismatch(request, expectedHex, computedHex,
        "Empreinte SHA-256 non conforme : image filesystem corrompue ou tronquée, redémarrage annulé");
      return;  // PAS de restart demandé
    }

    systemLogger.info("Mise à jour filesystem réussie (" + String(written) + " octets)");
    bool restartPlanned = shouldRestart && g_restartRequested != nullptr && g_restartRequestedTime != nullptr;
    if (!restartPlanned) {
      PumpController.setOtaInProgress(false);
    }

    // Remonter LittleFS
    if (!LittleFS.begin(false)) {
      systemLogger.warning("Impossible de remonter LittleFS (normal après mise à jour, redémarrage requis)");
    }

    if (shouldRestart && g_restartRequested != nullptr && g_restartRequestedTime != nullptr) {
      request->send(200, "application/json", "{\"status\":\"success\"}");
      *g_restartRequested = true;
      *g_restartRequestedTime = millis();
    } else {
      request->send(200, "application/json", "{\"status\":\"success\"}");
    }

  } else {
    // Mise à jour firmware: utiliser l'API Update standard
    if (!Update.begin(contentLength, U_FLASH)) {
      systemLogger.error("Erreur démarrage OTA: " + String(Update.errorString()));
      http.end();
      sendErrorResponse(request, 500, "OTA begin failed");
      return;
    }
    PumpController.setOtaInProgress(true);

    // Lire et écrire les données par blocs
    size_t yieldCounter = 0;
    while (http.connected() && (written < (size_t)contentLength)) {
      size_t available = stream->available();

      if (available) {
        int c = stream->readBytes(buff, min(available, sizeof(buff)));

        if (c > 0) {
          hasher.update(buff, c);  // Hachage incrémental du flux (feature-026)
          if (Update.write(buff, c) != (size_t)c) {
            // CAUSE CAPTURÉE AVANT abort() : abort() écrase le code réel par
            // UPDATE_ERROR_ABORT et la cause serait définitivement perdue.
            const String writeError = String(Update.errorString());
            Update.abort();
            systemLogger.error("Échec d'écriture de l'image firmware téléchargée (" +
                               writeError + ") — mise à jour interrompue");
            http.end();
            PumpController.setOtaInProgress(false);
            sendErrorResponse(request, 500, "OTA write failed: " + writeError);
            return;
          }
          written += c;
          yieldCounter++;

          // Yield et feed watchdog toutes les 8 écritures (~4KB)
          if (yieldCounter % 8 == 0) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(10));
          }

          // Log de progression tous les 100KB
          if (written % 102400 == 0 || written == (size_t)contentLength) {
            unsigned int percent = (written * 100) / contentLength;
            systemLogger.info("Téléchargement FW: " + String(percent) + "%");
            esp_task_wdt_reset();
          }
        }
      }
      delay(1);
    }

    http.end();

    // Vérification d'intégrité (feature-026) : comparaison AVANT tout
    // Update.end() — en cas de mismatch l'image n'est JAMAIS activée
    // (couvre aussi le téléchargement tronqué par une coupure WiFi).
    uint8_t computedFw[32];
    hasher.finish(computedFw);

    // feature-151 D1, lot B1 : comparaison 2/2 — voir le commentaire jumeau de
    // la branche filesystem ci-dessus. ⛔ JOURNALISE SEULEMENT.
    logOtaManifestBindingVerdict(
        "/download-update", "empreinte calculée (fichier firmware reçu)",
        otaManifestBindingVerdict(manifestDigestValid, manifestSha256Hex,
                                  /*comparedDigestValid=*/true, computedFw));

    if (!sha256Equal(computedFw, expectedHash)) {
      char expectedHex[kOtaSha256HexLen + 1];
      char computedHex[kOtaSha256HexLen + 1];
      sha256ToHex(expectedHash, expectedHex, sizeof(expectedHex));
      sha256ToHex(computedFw, computedHex, sizeof(computedHex));
      systemLogger.critical("Intégrité OTA FW: empreinte SHA-256 non conforme (attendue " +
                            String(expectedHex) + ", calculée " + String(computedHex) +
                            ") — flash refusé");
      Update.abort();  // AVANT tout end() : la partition OTA n'est pas validée
      PumpController.setOtaInProgress(false);  // Appariement du setOtaInProgress(true)
      sendIntegrityMismatch(request, expectedHex, computedHex,
        "Empreinte SHA-256 non conforme : image firmware corrompue ou tronquée, flash refusé");
      return;
    }

    // Finaliser la mise à jour
    if (Update.end(true)) {
      if (shouldRestart && g_restartRequested != nullptr && g_restartRequestedTime != nullptr) {
        systemLogger.info("Mise à jour firmware réussie! Redémarrage...");
        request->send(200, "application/json", "{\"status\":\"success\"}");
        *g_restartRequested = true;
        *g_restartRequestedTime = millis();
      } else {
        systemLogger.info("Mise à jour firmware réussie (sans redémarrage)");
        request->send(200, "application/json", "{\"status\":\"success\"}");
      }
      bool restartPlanned = shouldRestart && g_restartRequested != nullptr && g_restartRequestedTime != nullptr;
      if (!restartPlanned) {
        PumpController.setOtaInProgress(false);
      }
    } else {
      systemLogger.error("Erreur finalisation OTA: " + String(Update.errorString()));
      PumpController.setOtaInProgress(false);
      sendErrorResponse(request, 500, "OTA finalization failed");
    }
  }
}

void setupOtaRoutes(AsyncWebServer* server) {
  // Routes OTA - TOUTES PROTÉGÉES (CRITICAL)
  server->on("/check-update", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE); // Protection + rate limiting
    handleCheckUpdate(req);
  });

  server->on("/download-update", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL); // Téléchargement OTA critique
    handleDownloadUpdate(req);
  });

  // Route pour mise à jour OTA (firmware ou filesystem) - CRITIQUE
  server->on("/update", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      REQUIRE_AUTH(req, RouteProtection::CRITICAL);

      int status = 200;
      String body = "OK";

      // ORDRE IMPOSÉ (2.20.1) — le verdict mémorisé PASSE AVANT tout le reste.
      // 1) Un refus de type (pré-contrôle de l'octet magique) n'a JAMAIS appelé
      //    Update.begin() : Update.hasError() vaut false et le repli répondrait
      //    « OK » + redémarrage parasite sur un appareil qui n'a rien flashé.
      // 2) Ce chemin de refus pose aussi markUploadRejected() : sans ce test en
      //    premier, la branche 409 répondrait « une mise à jour OTA est déjà en
      //    cours », texte faux pour un refus de type.
      if (!takeOtaUploadVerdict(req, status, body)) {
        // Requête rejetée pour upload concurrent (marqueur posé par
        // markUploadRejected) : réponse d'erreur dédiée — Update.hasError()
        // reflète la session de l'AUTRE upload et donnerait un faux "OK".
        if (req->_tempObject != nullptr) {
          status = 409;
          body = "Upload refusé: une mise à jour OTA est déjà en cours";
        } else if (Update.hasError()) {
          // Échec sans verdict mémorisé (chemin résiduel) : au moins ne pas
          // mentir sur le code HTTP.
          status = 500;
          body = String("FAIL: échec de la mise à jour (") + Update.errorString() + ")";
        }
        // Sinon : chemin nominal INCHANGÉ → 200 / corps exactement "OK".
      }

      AsyncWebServerResponse *response = req->beginResponse(status, "text/plain", body);
      response->addHeader("Connection", "close");
      req->send(response);
      // Redémarrage planifié UNIQUEMENT sur succès réel.
      if (status == 200 && g_restartRequested != nullptr && g_restartRequestedTime != nullptr) {
        // Planifier le redémarrage dans update() pour ne pas bloquer
        *g_restartRequested = true;
        *g_restartRequestedTime = millis();
      }
    },
    handleOtaUpdate
  );
}
