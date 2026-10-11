#include "web_server.h"
#include "web_routes_config.h"
#include "web_routes_calibration.h"
#include "web_routes_control.h"
#include "web_routes_data.h"
#include "web_routes_ota.h"
#include "web_routes_auth.h"
#include "web_routes_coredump.h"
#include "web_routes_sensor_id.h"
#include "web_routes_debug.h"
#include "web_routes_heap.h"
#include "auth.h"
#include "config.h"
#include "constants.h"
#include "heap_metrics.h"        // heapLargestFreeBlock8Bit — garde 503 avant fopen (bug-080 B-3)
#include "http_lowheap_logic.h"  // décision pure + bornage du journal (ADR-0017)
#include "logger.h"
#include "mqtt_manager.h"
#include <LittleFS.h>
#include <atomic>

WebServerManager webServer;

// ---------------------------------------------------------------------------
// Garde 503 « fichiers statiques » (bug-080, lot B-3) — état partagé
// ---------------------------------------------------------------------------
// Écrit depuis `async_tcp` (filtre + callback du handler prioritaire), drainé
// depuis la loop. Deux atomiques relaxés, AUCUN mutex, AUCUNE tâche neuve : la
// seule contrainte est de ne jamais allouer ni journaliser côté `async_tcp`.
static std::atomic<uint32_t> sStaticRefusalCount{0};     // refus cumulés depuis le boot
// maxBloc8 de la dernière ÉVALUATION DE FILTRE sous le seuil — refus ou NON.
// Ce n'est PAS « la mesure du dernier refus » : _attachHandler (WebServer.cpp:147)
// évalue `h->filter(request) && h->canHandle(request)`, donc le filtre tourne
// AVANT le contrôle de méthode. Une requête POST qui atteint la position de la
// garde (les routes pompes dynamiques servies par onNotFound, p. ex.
// /pump1/duty/) exécute le filtre — et donc ce store — alors qu'elle ne sera
// jamais refusée. La valeur reste juste pour ce qu'on en fait (l'ordre de
// grandeur du tas au moment où la garde s'est armée), mais ne pas la lire comme
// un instantané du refus lui-même.
static std::atomic<uint32_t> sStaticRefusalMaxAlloc{0};
static HttpLowHeapEpisode sStaticRefusalEpisode;         // état d'épisode (loop uniquement)

WebServerManager::WebServerManager() {}

void WebServerManager::begin(AsyncWebServer* webServer, DNSServer* dnsServer) {
  server = webServer;
  dns = dnsServer;

  // Vérifier que LittleFS est monté
  if (!LittleFS.begin()) {
    systemLogger.critical("LittleFS non disponible pour le serveur Web");
    return;
  }

  setupRoutes();

  // Démarrer le serveur web
  server->begin();
  systemLogger.info("Serveur Web démarré sur le port 80");
}

// feature-118 lot B — PARADE CACHE RETIRÉE après audit security-reviewer P2.
// Une version intermédiaire remplaçait ici `req->send(LittleFS, …)` par un
// `beginResponse` + `addHeader("Cache-Control","no-cache")` manuel, pour
// corriger l'incident du 2026-10-11 (empreinte « absente » : page web obsolète).
// ⚠️ C'ÉTAIT REDONDANT **ET** RÉGRESSIF, et le coordinateur s'était trompé en
// le vérifiant (grep sur la chaîne "Cache-Control" alors que la bibliothèque
// utilise la constante `T_Cache_Control` — le symbole, pas le littéral) :
//   · `AsyncFileResponse` pose DÉJÀ `ETag` + `Cache-Control: no-cache`
//     (WebResponses.cpp:744-745) ;
//   · `req->send(FS, …)` teste DÉJÀ `If-None-Match` sur le repli gzip et peut
//     répondre 304 (AsyncWebServerRequest.cpp:26+) — que la version manuelle
//     PERDAIT ;
//   · `serveStatic` pose aussi `no-cache` par défaut (WebHandlers.cpp:287).
// ⇒ On garde `req->send(LittleFS, …)`. La cause réelle de l'incident n'était
// PAS un défaut d'en-tête mais un ONGLET DÉJÀ OUVERT avant le déploiement —
// aucun en-tête HTTP ne rattrape ça. Le remède est le rechargement, et le filet
// est le message `kOtaSha256MissingMessage` (src/constants.h), qui le dit.

void WebServerManager::setupRoutes() {
  // Pas de CORS : politique même-origine stricte (feature-028, option B).
  // L'UI est servie par l'ESP32 lui-même (offline first), aucun accès cross-origin supporté.

  // En-têtes de sécurité globaux (ajoutés à toutes les réponses)
  DefaultHeaders::Instance().addHeader("Content-Security-Policy",
    "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:");
  DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
  DefaultHeaders::Instance().addHeader("X-Frame-Options", "SAMEORIGIN");

  // Initialiser les contextes pour les modules qui ont besoin de partager des données
  initConfigContext(&configBuffers, &configErrors);
  initOtaContext(&restartRequested, &restartRequestedTime);

  // Configurer les routes par domaine fonctionnel
  setupAuthRoutes(server);  // Routes d'authentification (login, changement mot de passe, token)
  setupDataRoutes(server);
  setupConfigRoutes(server, &restartApRequested, &restartRequestedTime);
  setupCalibrationRoutes(server);
  setupControlRoutes(server);
  setupOtaRoutes(server);
  setupCoredumpRoutes(server);
  setupSensorIdRoutes(server);  // feature-020 : identification 2 sondes DS18B20
  setupDebugRoutes(server);     // ph_slope_refresh + sensor_filter_reset/state (levier sécurité feature-025)
  setupHeapRoutes(server);      // feature-128 : recensement du tas (R2) + lecture de contenu (R3, S7/I9 : AVANT la garde 503 ci-dessous)

  // WebSocket (push temps réel : capteurs toutes les 5s, config après save, logs en direct)
  wsManager.begin(server);

  // ==========================================================================
  // GARDE 503 « fichiers statiques » — bug-080, lot B-3 (v2.41.0)
  // ==========================================================================
  // Position DÉLIBÉRÉE, ne pas déplacer : APRÈS wsManager.begin(), AVANT
  // /login.html. C'est cet ordre — et lui seul — qui fait la politique
  // d'exemption. _attachHandler (WebServer.cpp:145-152) parcourt _handlers dans
  // l'ORDRE D'ENREGISTREMENT et s'arrête au premier filter() && canHandle() :
  // tout ce qui est déclaré au-dessus (data, config, calibration, control, OTA,
  // coredump, sondes, debug, auth, /ws) reste servi même en famine, et tout ce
  // qui est déclaré en dessous (les 7 pages HTML + serveStatic) est couvert.
  //
  // ⚠️ TOUTE ROUTE `HTTP_GET` AJOUTÉE APRÈS CE POINT SERA COUVERTE PAR LA GARDE
  // et répondra 503 sous 8 192 o de plus gros bloc. Une route GET qui doit
  // rester servie en famine (diagnostic, secours) doit être enregistrée AVANT,
  // avec les autres routes JSON. Aucune liste d'exemption n'est maintenue : une
  // liste s'oublie, l'ordre d'enregistrement se voit.
  //
  // Pourquoi un handler PRIORITAIRE et non un filtre sur serveStatic :
  // AsyncStaticWebHandler::canHandle (WebHandlers.cpp:105-107) appelle _getFile()
  // → _searchFile(), donc serveStatic TOUCHE LE FS DÈS canHandle. Seul un
  // handler antérieur est réellement en amont du fopen().
  //
  // Les routes dynamiques des pompes (onNotFound) sont HTTP_POST : hors
  // matcher, donc jamais interceptées — aucune commande de dosage ne peut être
  // avalée par cette garde GET-only.
  // NUANCE À NE PAS PERDRE : le matcher protège de l'INTERCEPTION, pas de
  // l'ÉVALUATION. _attachHandler (WebServer.cpp:147) teste
  // `h->filter(request) && h->canHandle(request)` : le filtre passe AVANT le
  // contrôle de méthode, donc un POST qui remonte jusqu'ici EXÉCUTE le filtre —
  // parcours du tas compris, et écriture de sStaticRefusalMaxAlloc. Coût arbitré
  // (70-250 µs/appel, ≲ 5-10 % du service de fichier qui existe déjà) ; effet de
  // bord assumé et documenté à la déclaration de l'atomique. La requête POST,
  // elle, poursuit son chemin normalement.
  server->on(AsyncURIMatcher::all(), HTTP_GET, [](AsyncWebServerRequest* req) {
    // Contexte : tâche `async_tcp`. Un log ici serait potentiellement un
    // ERROR/CRITICAL → flushToDisk() synchrone → fopen() → l'abort() même que
    // l'on prévient. Donc : incrément atomique, réponse, rien d'autre. Zéro
    // String, zéro allocation hors la réponse (littéraux .rodata).
    //
    // POURQUOI `relaxed` ET PAS `release`/`acquire` — analyse déjà faite, ne pas
    // la refaire : au sens STRICT du modèle mémoire C++, rien n'apparie ce
    // compteur au store de sStaticRefusalMaxAlloc (filtre), donc la loop pourrait
    // en théorie voir l'incrément sans le maxAlloc associé. C'est sûr ici pour
    // trois raisons cumulées : (1) les deux écritures viennent de la MÊME tâche
    // (`async_tcp`), dans cet ordre, à quelques microsecondes d'intervalle ;
    // (2) elles sont dans deux lambdas appelées par indirection `std::function`,
    // que le compilateur ne peut pas réordonner l'une par rapport à l'autre ;
    // (3) le LX6 ne réordonne pas les accès mémoire de façon observable.
    // Et surtout : le pire cas est une ligne de journal affichant un maxBloc8
    // d'une évaluation voisine — jamais une décision fausse, le prédicat de refus
    // ne lisant aucune de ces deux variables. Le coût d'une barrière sur le
    // chemin chaud d'`async_tcp` n'est pas justifié par cet enjeu.
    sStaticRefusalCount.fetch_add(1, std::memory_order_relaxed);
    AsyncWebServerResponse* res =
      req->beginResponse(503, "text/plain", "Memoire insuffisante, reessayez");
    // 5 s : sous le débounce d'auto-guérison (~15 s) et au-dessus de l'échelle
    // d'une rafale de chargement de page. Coût mesuré : 56 o de flash — conservé,
    // c'est ce qui empêche un navigateur (ou Home Assistant) de marteler l'ESP32
    // pendant la famine même qu'on essaie de laisser se résorber.
    res->addHeader("Retry-After", "5");
    req->send(res);
  }).setFilter([](AsyncWebServerRequest*) {
    // Aussi sur `async_tcp` : heapLargestFreeBlock8Bit() seulement (sans log ni
    // allocation), JAMAIS heapMetricsSnapshot().
    const uint32_t maxAlloc8 = heapLargestFreeBlock8Bit();
    // feature-122 (A1) : la valeur est DÉJÀ mesurée pour la décision — on la
    // pousse vers le plancher glissant 0x0804 (fetch_min) et on compte
    // l'évaluation. AUCUN parcours de tas neuf, aucun log, aucune allocation :
    // contrat de `async_tcp`. C'est ce qui donne au plancher une résolution à la
    // REQUÊTE : une rafale de chargement d'interface dure moins qu'un intervalle
    // d'échantillonnage (2 s), donc un plancher purement périodique la raterait.
    // ⚠️ Le SEUIL de la garde reste kStaticFileMinAllocBytes et sa MÉTRIQUE reste
    // 0x0804 : ce lot mesure, il ne corrige pas (la bascule éventuelle vers
    // 0x1804 est feature-123, avec son ADR).
    heapNoteHttpGetSample(maxAlloc8);
    if (!httpLowHeapShouldRefuse(maxAlloc8, kStaticFileMinAllocBytes)) return false;
    // `relaxed` : justification appariée au fetch_add ci-dessus. Rappel — ce
    // store s'exécute aussi pour un POST qui atteint la garde (le filtre précède
    // le contrôle de méthode) ; il n'implique donc pas qu'un refus a suivi.
    sStaticRefusalMaxAlloc.store(maxAlloc8, std::memory_order_relaxed);
    return true;
  });

  // Page de login (PUBLIC - doit être accessible sans auth)
  server->on("/login.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/login.html", "text/html");
  });

  server->on("/wifi.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/wifi.html", "text/html");
  });

  // Pages HTML (PUBLIC - le JavaScript vérifie l'auth au chargement via sessionStorage)
  // Les pages HTML ne peuvent pas vérifier le token car les navigateurs n'envoient pas
  // de headers personnalisés lors des navigations (redirections, liens directs, etc.)
  server->on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/index.html", "text/html");
  });

  server->on("/wizard.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/wizard.html", "text/html");
  });

  server->on("/index.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/index.html", "text/html");
  });

  server->on("/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/config.html", "text/html");
  });

  server->on("/config.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/config.html", "text/html");
  });

  // Fichiers statiques (CSS, JS, images) - PUBLIC pour que la page de login fonctionne
  // .setCacheControl("no-cache") — feature-118 lot B : EXPLICITE plutôt que de
  // compter sur le défaut de la bibliothèque (AsyncStaticWebHandler::handleRequest,
  // WebHandlers.cpp) qui pose déjà "no-cache" en l'absence d'appel à cette
  // méthode — ce chemin EST celui qui teste `If-None-Match` et peut répondre
  // 304 (seul endroit du firmware où un 304 est possible).
  server->serveStatic("/", LittleFS, "/")
    .setDefaultFile("index.html")
    .setCacheControl("no-cache")
    .setFilter([](AsyncWebServerRequest *req) {
      String path = req->url();
      // Autoriser les assets (CSS, JS, images, fonts)
      if (path.endsWith(".css") || path.endsWith(".js") ||
          path.endsWith(".png") || path.endsWith(".jpg") || path.endsWith(".jpeg") ||
          path.endsWith(".gif") || path.endsWith(".ico") || path.endsWith(".svg") ||
          path.endsWith(".woff") || path.endsWith(".woff2") || path.endsWith(".ttf")) {
        return true;
      }
      // Bloquer les fichiers HTML (gérés par les routes ci-dessus)
      return false;
    });

  // Handler global pour routes dynamiques et 404
  // IMPORTANT: Doit être déclaré en DERNIER pour ne pas intercepter les autres routes
  server->onNotFound([](AsyncWebServerRequest *req) {
    // Essayer les routes dynamiques des pompes
    if (handleDynamicPumpRoutes(req)) {
      return;
    }

    // 404 pour les autres routes non trouvées
    req->send(404, "text/plain", "Not Found");
  });
}

// Drain du compteur de refus de la garde 503 — exécuté depuis la LOOP.
// Deux lignes de journal par épisode, pas une par requête : une rafale de
// chargement de page produit 6 à 20 refus, et un journal persistant de 8 Ko a
// 25-35 min de rétention. Journaliser par requête effacerait la fenêtre
// forensique de l'épisode qu'on cherche justement à documenter (leçon du 15/08).
// NIVEAUX : WARN et INFO exclusivement. Un ERROR/CRITICAL déclencherait le
// flushToDisk() synchrone de logger.cpp:71-73 → fopen() → l'abort() prévenu.
void WebServerManager::drainLowHeapGuard() {
  const uint32_t now = millis();
  const uint32_t refusals = sStaticRefusalCount.load(std::memory_order_relaxed);

  // Le tas n'est mesuré que si la fermeture d'épisode est réellement en jeu :
  // pas de parcours du tas à la fréquence de la loop pendant la famine.
  const uint32_t maxAlloc8 =
    httpLowHeapNeedsHeapSample(sStaticRefusalEpisode, now, kHttpLowHeapEpisodeQuietMs)
      ? heapLargestFreeBlock8Bit()
      : 0;

  const HttpLowHeapDrainResult verdict =
    httpLowHeapDrain(sStaticRefusalEpisode, refusals, now, maxAlloc8,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);

  if (verdict.action == HttpLowHeapLogAction::None) return;

  // UN SEUL site d'appel snprintf, sur tampon de PILE, pour les deux fronts :
  // les deux lignes prennent exactement deux %u, on sélectionne donc le format
  // et les arguments plutôt que de dupliquer l'appel (économie de flash sur une
  // partition à 99,5 %). Le seul temporaire qui subsiste est la String qu'impose
  // l'API du logger.
  const bool debut = (verdict.action == HttpLowHeapLogAction::EpisodeStart);
  char ligne[112];
  snprintf(ligne, sizeof(ligne),
           debut ? "Service fichiers suspendu (503) — maxBloc8=%u o < %u o"
                 : "Service fichiers rétabli — %u refus en %u s",
           debut ? (unsigned)sStaticRefusalMaxAlloc.load(std::memory_order_relaxed)
                 : (unsigned)verdict.refusals,
           debut ? (unsigned)kStaticFileMinAllocBytes
                 : (unsigned)(verdict.durationMs / 1000UL));
  if (debut) {
    systemLogger.warning(ligne);  // JAMAIS error() : flush disque synchrone → fopen()
  } else {
    systemLogger.info(ligne);
  }
}

void WebServerManager::update() {
  wsManager.update();
  drainLowHeapGuard();  // bug-080 B-3 : bornage du journal de la garde 503

  // Gérer le redémarrage après OTA (attendre que la réponse HTTP soit envoyée)
  if (restartRequested && (millis() - restartRequestedTime >= kRestartAfterOtaDelayMs)) {
    restartRequested = false;
    systemLogger.critical("Redémarrage après mise à jour OTA");
    // v2.27.0 (gate pool-chemistry) : seul performFailSafeRestart() persistait les
    // compteurs journaliers. Un redémarrage OTA perdait donc jusqu'à 30 s de
    // comptage — et surtout, l'épisode d'OTA lui-même laissait courir les minutes
    // de circulation. Les poser ICI garde le rapport mL/min cohérent au reboot.
    saveDailyCounters();
    mqttManager.shutdownForRestart();  // ADR-0011 : flush status=offline + stop mqttTask
    ESP.restart();
  }

  // Gérer le redémarrage en mode AP
  if (restartApRequested && (millis() - restartRequestedTime >= kRestartApModeDelayMs)) {
    restartApRequested = false;
    systemLogger.critical("Redémarrage en mode Point d'accès");
    saveDailyCounters();  // idem : ne pas perdre le dénominateur de charge
    mqttManager.shutdownForRestart();  // ADR-0011 : flush status=offline + stop mqttTask
    ESP.restart();
  }
}
