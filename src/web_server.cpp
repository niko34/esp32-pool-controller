#include "web_server.h"
#include "web_routes_config.h"
#include "web_routes_calibration.h"
#include "web_routes_control.h"
#include "web_routes_data.h"
#include "web_routes_ota.h"
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

  // Ajouter les en-têtes CORS pour toutes les requêtes
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  setupRoutes();

  // Note: Ne pas appeler server->begin() ici car AsyncWiFiManager
  // a déjà appelé begin() sur le serveur lors de l'autoConnect().
  // Appeler begin() deux fois peut causer des conflits.
  systemLogger.info("Routes du serveur Web configurées");
}

void WebServerManager::setupRoutes() {
  // Initialiser les contextes pour les modules qui ont besoin de partager des données
  initConfigContext(&configBuffers, &configErrors);
  initOtaContext(&restartRequested, &restartRequestedTime);

  // Configurer les routes par domaine fonctionnel
  setupDataRoutes(server);
  setupConfigRoutes(server, &restartApRequested, &restartRequestedTime);
  setupCalibrationRoutes(server);
  setupControlRoutes(server);
  setupOtaRoutes(server);

  // Page de configuration (fichier statique)
  server->on("/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(LittleFS, "/config.html", "text/html");
  });

  // Handler global pour CORS OPTIONS, routes dynamiques et 404
  server->onNotFound([](AsyncWebServerRequest *req) {
    // Gérer CORS OPTIONS
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

  // Servir les fichiers statiques (HTML, CSS, JS, images)
  server->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
}

void WebServerManager::update() {
  // Gérer le redémarrage après OTA (attendre 3s pour que la réponse HTTP soit envoyée)
  if (restartRequested && (millis() - restartRequestedTime >= 3000)) {
    restartRequested = false;
    systemLogger.critical("Redémarrage après mise à jour OTA");
    ESP.restart();
  }

  // Gérer le redémarrage en mode AP (attendre 1s)
  if (restartApRequested && (millis() - restartRequestedTime >= 1000)) {
    restartApRequested = false;
    systemLogger.critical("Redémarrage en mode Point d'accès");
    ESP.restart();
  }
}
