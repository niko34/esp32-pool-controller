#include "web_routes_auth.h"
#include "web_helpers.h"
#include "auth.h"
#include "config.h"
#include <ArduinoJson.h>
#include <WiFi.h>

void setupAuthRoutes(AsyncWebServer* server) {
  // Route: Statut d'authentification (PUBLIC - vérifier si premier démarrage)
  server->on("/auth/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["firstBoot"] = authManager.isFirstBootDetected();
    doc["authEnabled"] = authManager.isEnabled();
    doc["forceWifiConfig"] = authCfg.forceWifiConfig;
    sendJsonResponse(req, doc);
  });

  // Route: Login (PUBLIC - génère un token de session)
  server->on("/auth/login", HTTP_POST, [](AsyncWebServerRequest *req) {}, nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!authManager.checkRateLimit(req)) {
        authManager.sendRateLimitExceeded(req);
        return;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        sendErrorResponse(req, 400, "Invalid JSON");
        return;
      }

      String username = doc["username"] | "";
      String password = doc["password"] | "";

      if (username != "admin" || password != authManager.getPassword()) {
        sendErrorResponse(req, 401, "Nom d'utilisateur ou mot de passe invalide");
        return;
      }

      // SÉCURITÉ: Bloquer le login si premier démarrage (mot de passe par défaut)
      // L'utilisateur DOIT changer le mot de passe via /auth/change-password d'abord
      if (authManager.isFirstBootDetected()) {
        sendErrorResponse(req, 403, "Changement de mot de passe obligatoire au premier démarrage");
        return;
      }

      // Authentification réussie - retourner le token API
      JsonDocument response;
      response["success"] = true;
      response["token"] = authManager.getApiToken();
      response["username"] = "admin";
      sendJsonResponse(req, response);
    }
  );

  // Route: Changement de mot de passe (PUBLIC pour premier démarrage, sinon AUTH)
  server->on("/auth/change-password", HTTP_POST, [](AsyncWebServerRequest *req) {}, nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!authManager.checkRateLimit(req)) {
        authManager.sendRateLimitExceeded(req);
        return;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        sendErrorResponse(req, 400, "Invalid JSON");
        return;
      }

      String currentPassword = doc["currentPassword"] | "";
      String newPassword = doc["newPassword"] | "";

      // Logique d'authentification :
      // 1. Si premier démarrage (firstBoot) : autoriser sans authentification
      // 2. Si currentPassword fourni : vérifier qu'il correspond
      // 3. Si currentPassword vide : vérifier que l'utilisateur est déjà authentifié

      bool isFirstBoot = authManager.isFirstBootDetected();
      bool isAuthenticated = false;

      if (!isFirstBoot) {
        // Pas en premier démarrage : authentification requise
        if (currentPassword.isEmpty()) {
          // Pas de currentPassword : vérifier l'authentification par token
          isAuthenticated = authManager.checkAuth(req, RouteProtection::WRITE);
          if (!isAuthenticated) {
            sendErrorResponse(req, 401, "Authentication required");
            return;
          }
        } else {
          // currentPassword fourni : vérifier qu'il correspond
          if (currentPassword != authManager.getPassword()) {
            sendErrorResponse(req, 401, "Current password incorrect");
            return;
          }
        }
      } else {
        // Premier démarrage : vérifier que le currentPassword est "admin"
        if (!currentPassword.isEmpty() && currentPassword != "admin") {
          sendErrorResponse(req, 401, "Current password incorrect");
          return;
        }
      }

      // Vérifier la longueur du nouveau mot de passe
      if (newPassword.length() < 8) {
        sendErrorResponse(req, 400, "New password must be at least 8 characters");
        return;
      }

      // Vérifier que le nouveau mot de passe est différent (seulement si currentPassword fourni)
      if (!currentPassword.isEmpty() && newPassword == currentPassword) {
        sendErrorResponse(req, 400, "New password must be different from current password");
        return;
      }

      // Changer le mot de passe
      authCfg.adminPassword = newPassword;
      authManager.setPassword(newPassword);

      // Si c'était un reset password, désactiver le flag forceWifiConfig
      // L'utilisateur a maintenant défini un nouveau mot de passe sécurisé
      // Le mode AP sera désactivé automatiquement après connexion si WiFi est connecté
      if (authCfg.forceWifiConfig) {
        authCfg.forceWifiConfig = false;
      }

      saveMqttConfig();

      // Retourner le token API pour connexion automatique
      JsonDocument response;
      response["success"] = true;
      response["token"] = authManager.getApiToken();
      response["message"] = "Password changed successfully";
      sendJsonResponse(req, response);
    }
  );

  // Route: Régénération du token API (PROTÉGÉE - nécessite auth)
  server->on("/auth/regenerate-token", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    // Régénérer le token
    authManager.regenerateApiToken();
    authCfg.apiToken = authManager.getApiToken();
    saveMqttConfig();

    JsonDocument doc;
    doc["success"] = true;
    doc["token"] = authManager.getApiToken();
    doc["message"] = "API token regenerated";
    sendJsonResponse(req, doc);
  });

  // Route: Obtenir le token API actuel (PROTÉGÉE - nécessite auth)
  server->on("/auth/token", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    JsonDocument doc;
    doc["token"] = authManager.getApiToken();
    sendJsonResponse(req, doc);
  });
}
