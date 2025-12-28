#ifndef AUTH_H
#define AUTH_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <map>
#include <vector>
#include "constants.h"

// Niveau de protection des routes
enum class RouteProtection {
  NONE,      // Route publique (lecture seule)
  WRITE,     // Route d'écriture (nécessite auth)
  CRITICAL   // Route critique (admin uniquement)
};

// Structure pour le rate limiting
struct RateLimitEntry {
  unsigned long firstRequestTime;
  uint16_t requestCount;
};

class AuthManager {
private:
  // Configuration
  bool authEnabled = true;
  String adminPassword = "";
  String apiToken = "";
  bool isFirstBoot = false;  // Premier démarrage (mot de passe par défaut)

  // Rate limiting (IP -> stats)
  std::map<String, RateLimitEntry> rateLimitMap;
  static const uint16_t MAX_REQUESTS_PER_MINUTE = kMaxRequestsPerMinute;
  static const unsigned long RATE_LIMIT_WINDOW_MS = kRateLimitWindowMs;

  // Génération de token aléatoire
  String generateRandomToken();

  // Nettoyage des entrées de rate limiting expirées
  void cleanupRateLimitMap();

public:
  AuthManager();

  // Initialisation
  void begin();

  // Configuration
  void setEnabled(bool enabled) { authEnabled = enabled; }
  bool isEnabled() const { return authEnabled; }
  void setPassword(const String& pwd);
  String getPassword() const { return adminPassword; }
  void setApiToken(const String& token);
  String getApiToken() const { return apiToken; }
  void regenerateApiToken();

  // Gestion premier démarrage
  bool isFirstBootDetected() const { return isFirstBoot; }
  void clearFirstBootFlag() { isFirstBoot = false; }

  // Réinitialisation mot de passe (bouton physique)
  void resetPasswordToDefault();

  // Vérification d'authentification
  bool checkAuth(AsyncWebServerRequest* req, RouteProtection level = RouteProtection::WRITE);

  // HTTP Basic Auth
  bool checkBasicAuth(AsyncWebServerRequest* req);

  // API Token Auth
  bool checkTokenAuth(AsyncWebServerRequest* req);

  // Rate limiting
  bool checkRateLimit(AsyncWebServerRequest* req);

  // Helper pour envoyer une réponse d'erreur d'auth
  void sendAuthRequired(AsyncWebServerRequest* req);
  void sendRateLimitExceeded(AsyncWebServerRequest* req);
};

// Instance globale
extern AuthManager authManager;

// Middleware helper pour protéger les routes
#define REQUIRE_AUTH(req, level) \
  if (!authManager.checkAuth(req, level)) { \
    return; \
  }

#endif // AUTH_H
