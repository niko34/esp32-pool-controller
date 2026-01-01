#include "web_server.h"
#include "web_routes_config.h"
#include "web_routes_calibration.h"
#include "web_routes_control.h"
#include "web_routes_data.h"
#include "web_routes_ota.h"
#include "web_routes_auth.h"
#include "auth.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include <LittleFS.h>

WebServerManager webServer;

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

void WebServerManager::setCorsHeaders(AsyncWebServerRequest* req) {
  // CORS restrictif avec liste blanche configurable
  // Si corsAllowedOrigins est vide ou "*", utiliser le wildcard (moins sécurisé mais simple)
  if (authCfg.corsAllowedOrigins.isEmpty()) {
    return; // Pas de CORS
  }

  if (authCfg.corsAllowedOrigins == "*") {
    // Wildcard - accepter toutes les origines (moins sécurisé)
    return; // Géré par DefaultHeaders
  }

  // Récupérer l'origin de la requête
  if (!req->hasHeader("Origin")) {
    return; // Pas d'origin header, pas besoin de CORS
  }

  String origin = req->getHeader("Origin")->value();

  // Vérifier si l'origin est dans la liste blanche (séparée par virgules)
  bool allowed = false;
  int startPos = 0;
  while (startPos < authCfg.corsAllowedOrigins.length()) {
    int commaPos = authCfg.corsAllowedOrigins.indexOf(',', startPos);
    if (commaPos == -1) commaPos = authCfg.corsAllowedOrigins.length();

    String allowedOrigin = authCfg.corsAllowedOrigins.substring(startPos, commaPos);
    allowedOrigin.trim();

    if (origin == allowedOrigin) {
      allowed = true;
      break;
    }

    startPos = commaPos + 1;
  }

  // Si l'origin n'est pas autorisée, ne rien faire (CORS bloquera la requête côté navigateur)
  if (!allowed) {
    systemLogger.warning("CORS: Origin non autorisée: " + origin);
  }
}

void WebServerManager::setupRoutes() {
  // CORS: Configurer les headers selon authCfg.corsAllowedOrigins
  // Si vide : pas de CORS
  // Si "*" : wildcard (tous autorisés - moins sécurisé)
  // Sinon : liste blanche (vérification par setCorsHeaders dans chaque route)

  if (!authCfg.corsAllowedOrigins.isEmpty()) {
    if (authCfg.corsAllowedOrigins == "*") {
      // Mode wildcard (moins sécurisé mais compatible avec tout)
      DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
      systemLogger.warning("CORS: Mode wildcard activé (*) - Moins sécurisé !");
    } else {
      // Mode liste blanche - on autorise toutes les origines configurées
      // Note: Avec ESPAsyncWebServer, on ne peut pas facilement faire du CORS dynamique par requête
      // On utilise donc le premier origin de la liste comme fallback
      int firstComma = authCfg.corsAllowedOrigins.indexOf(',');
      String firstOrigin = (firstComma > 0)
        ? authCfg.corsAllowedOrigins.substring(0, firstComma)
        : authCfg.corsAllowedOrigins;
      firstOrigin.trim();
      DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", firstOrigin);
      systemLogger.info("CORS: Origines autorisées: " + authCfg.corsAllowedOrigins);
    }

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, X-Auth-Token, Authorization");
  } else {
    systemLogger.info("CORS désactivé (pas d'origines configurées)");
  }

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
  server->serveStatic("/", LittleFS, "/")
    .setDefaultFile("index.html")
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

  // Handler global pour CORS preflight, routes dynamiques et 404
  // IMPORTANT: Doit être déclaré en DERNIER pour ne pas intercepter les autres routes
  server->onNotFound([](AsyncWebServerRequest *req) {
    // Gérer CORS preflight OPTIONS
    if (req->method() == HTTP_OPTIONS) {
      req->send(200);
      return;
    }

    // Essayer les routes dynamiques des pompes
    if (handleDynamicPumpRoutes(req)) {
      return;
    }

    // 404 pour les autres routes non trouvées
    req->send(404, "text/plain", "Not Found");
  });
}

void WebServerManager::update() {
  // Gérer le redémarrage après OTA (attendre que la réponse HTTP soit envoyée)
  if (restartRequested && (millis() - restartRequestedTime >= kRestartAfterOtaDelayMs)) {
    restartRequested = false;
    systemLogger.critical("Redémarrage après mise à jour OTA");
    ESP.restart();
  }

  // Gérer le redémarrage en mode AP
  if (restartApRequested && (millis() - restartRequestedTime >= kRestartApModeDelayMs)) {
    restartApRequested = false;
    systemLogger.critical("Redémarrage en mode Point d'accès");
    ESP.restart();
  }
}
