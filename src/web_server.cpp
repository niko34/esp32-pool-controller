#include "web_server.h"
#include "web_routes_config.h"
#include "web_routes_calibration.h"
#include "web_routes_control.h"
#include "web_routes_data.h"
#include "web_routes_ota.h"
#include "web_routes_auth.h"
#include "auth.h"
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

void WebServerManager::setupRoutes() {
  // CORS: Autoriser toutes les origines pour Home Assistant et accès Internet
  // L'authentification se fait via token API (X-Auth-Token), pas via cookies
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, X-Auth-Token, Authorization");

  // Note: credentials:true incompatible avec origin:*, on utilise token auth à la place

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

  // Page principale (PROTÉGÉE - redirige vers login si non auth)
  server->on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!authManager.checkAuth(req, RouteProtection::NONE)) {
      req->redirect("/login.html");
      return;
    }
    req->send(LittleFS, "/index.html", "text/html");
  });

  server->on("/index.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!authManager.checkAuth(req, RouteProtection::NONE)) {
      req->redirect("/login.html");
      return;
    }
    req->send(LittleFS, "/index.html", "text/html");
  });

  // Page de configuration (PROTÉGÉE - redirige vers login si non auth)
  server->on("/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!authManager.checkAuth(req, RouteProtection::NONE)) {
      req->redirect("/login.html");
      return;
    }
    req->send(LittleFS, "/config.html", "text/html");
  });

  server->on("/config.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!authManager.checkAuth(req, RouteProtection::NONE)) {
      req->redirect("/login.html");
      return;
    }
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
