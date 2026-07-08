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
#include "auth.h"
#include "config.h"
#include "constants.h"
#include "logger.h"
#include "mqtt_manager.h"
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

  // WebSocket (push temps réel : capteurs toutes les 5s, config après save, logs en direct)
  wsManager.begin(server);

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

void WebServerManager::update() {
  wsManager.update();

  // Gérer le redémarrage après OTA (attendre que la réponse HTTP soit envoyée)
  if (restartRequested && (millis() - restartRequestedTime >= kRestartAfterOtaDelayMs)) {
    restartRequested = false;
    systemLogger.critical("Redémarrage après mise à jour OTA");
    mqttManager.shutdownForRestart();  // ADR-0011 : flush status=offline + stop mqttTask
    ESP.restart();
  }

  // Gérer le redémarrage en mode AP
  if (restartApRequested && (millis() - restartRequestedTime >= kRestartApModeDelayMs)) {
    restartApRequested = false;
    systemLogger.critical("Redémarrage en mode Point d'accès");
    mqttManager.shutdownForRestart();  // ADR-0011 : flush status=offline + stop mqttTask
    ESP.restart();
  }
}
