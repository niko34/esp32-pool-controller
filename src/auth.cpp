#include "auth.h"
#include "logger.h"
#include <esp_random.h>

AuthManager authManager;

AuthManager::AuthManager() {}

void AuthManager::begin() {
  // Générer un token API par défaut si vide
  if (apiToken.isEmpty()) {
    apiToken = generateRandomToken();
    systemLogger.info("API Token généré: " + apiToken);
  }

  // Générer un mot de passe par défaut si vide
  if (adminPassword.isEmpty()) {
    adminPassword = "admin";
    systemLogger.warning("Mot de passe par défaut utilisé. Changez-le dans la configuration !");
  }

  if (authEnabled) {
    systemLogger.info("Authentification activée (HTTP Basic + API Token)");
  } else {
    systemLogger.warning("Authentification désactivée - Mode ouvert !");
  }
}

String AuthManager::generateRandomToken() {
  // Générer un token de 32 caractères hexadécimaux
  String token = "";
  for (int i = 0; i < 16; i++) {
    uint8_t randomByte = esp_random() & 0xFF;
    char hex[3];
    sprintf(hex, "%02x", randomByte);
    token += hex;
  }
  return token;
}

void AuthManager::setPassword(const String& pwd) {
  adminPassword = pwd;
  systemLogger.info("Mot de passe administrateur modifié");
}

void AuthManager::setApiToken(const String& token) {
  apiToken = token;
  systemLogger.info("API Token modifié");
}

void AuthManager::regenerateApiToken() {
  apiToken = generateRandomToken();
  systemLogger.info("Nouveau API Token généré: " + apiToken);
}

bool AuthManager::checkBasicAuth(AsyncWebServerRequest* req) {
  // Vérifier HTTP Basic Auth
  if (!req->authenticate("admin", adminPassword.c_str())) {
    return false;
  }
  return true;
}

bool AuthManager::checkTokenAuth(AsyncWebServerRequest* req) {
  // Vérifier le header X-Auth-Token
  if (req->hasHeader("X-Auth-Token")) {
    const AsyncWebHeader* header = req->getHeader("X-Auth-Token");
    if (header->value() == apiToken) {
      return true;
    }
  }

  // Vérifier le query parameter ?token=...
  if (req->hasParam("token")) {
    const AsyncWebParameter* param = req->getParam("token");
    if (param->value() == apiToken) {
      return true;
    }
  }

  return false;
}

bool AuthManager::checkRateLimit(AsyncWebServerRequest* req) {
  // Nettoyer les entrées expirées toutes les 100 requêtes
  static uint16_t requestCounter = 0;
  if (++requestCounter >= 100) {
    cleanupRateLimitMap();
    requestCounter = 0;
  }

  // Obtenir l'IP du client
  String clientIP = req->client()->remoteIP().toString();
  unsigned long now = millis();

  // Vérifier ou créer l'entrée de rate limiting
  auto it = rateLimitMap.find(clientIP);
  if (it == rateLimitMap.end()) {
    // Première requête de cette IP
    rateLimitMap[clientIP] = {now, 1};
    return true;
  }

  RateLimitEntry& entry = it->second;

  // Vérifier si la fenêtre a expiré
  if (now - entry.firstRequestTime >= RATE_LIMIT_WINDOW_MS) {
    // Nouvelle fenêtre
    entry.firstRequestTime = now;
    entry.requestCount = 1;
    return true;
  }

  // Incrémenter le compteur
  entry.requestCount++;

  // Vérifier la limite
  if (entry.requestCount > MAX_REQUESTS_PER_MINUTE) {
    systemLogger.warning("Rate limit dépassé pour " + clientIP + " (" + String(entry.requestCount) + " req/min)");
    return false;
  }

  return true;
}

void AuthManager::cleanupRateLimitMap() {
  unsigned long now = millis();
  std::vector<String> toRemove;

  for (auto& pair : rateLimitMap) {
    if (now - pair.second.firstRequestTime >= RATE_LIMIT_WINDOW_MS) {
      toRemove.push_back(pair.first);
    }
  }

  for (const String& ip : toRemove) {
    rateLimitMap.erase(ip);
  }

  if (!toRemove.empty()) {
    systemLogger.debug("Rate limit: " + String(toRemove.size()) + " entrées nettoyées");
  }
}

bool AuthManager::checkAuth(AsyncWebServerRequest* req, RouteProtection level) {
  // Si l'authentification est désactivée, autoriser
  if (!authEnabled) {
    return true;
  }

  // Les routes publiques ne nécessitent pas d'auth
  if (level == RouteProtection::NONE) {
    return true;
  }

  // Vérifier le rate limiting d'abord (même pour les requêtes authentifiées)
  if (!checkRateLimit(req)) {
    sendRateLimitExceeded(req);
    return false;
  }

  // Vérifier l'authentification (Token en priorité, puis Basic Auth)
  bool authenticated = checkTokenAuth(req) || checkBasicAuth(req);

  if (!authenticated) {
    sendAuthRequired(req);
    return false;
  }

  return true;
}

void AuthManager::sendAuthRequired(AsyncWebServerRequest* req) {
  // Enregistrer la tentative
  String clientIP = req->client()->remoteIP().toString();
  systemLogger.warning("Accès non autorisé depuis " + clientIP + " vers " + req->url());

  // Envoyer la réponse 401 avec demande Basic Auth
  AsyncWebServerResponse* response = req->beginResponse(401, "application/json", "{\"error\":\"Authentication required\"}");
  response->addHeader("WWW-Authenticate", "Basic realm=\"Pool Controller\"");
  req->send(response);
}

void AuthManager::sendRateLimitExceeded(AsyncWebServerRequest* req) {
  String clientIP = req->client()->remoteIP().toString();
  systemLogger.warning("Rate limit dépassé pour " + clientIP);

  AsyncWebServerResponse* response = req->beginResponse(429, "application/json", "{\"error\":\"Too many requests\"}");
  response->addHeader("Retry-After", "60");
  req->send(response);
}
