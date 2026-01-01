#include "web_routes_ota.h"
#include "web_helpers.h"
#include "constants.h"
#include "auth.h"
#include "logger.h"
#include "version.h"
#include "github_root_ca.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <time.h>

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
}

static bool isTimeSynchronized() {
  time_t now = time(nullptr);
  return now > 1609459200; // 2021-01-01
}

static bool ensureTimeForTls(AsyncWebServerRequest* request) {
  if (!isTimeSynchronized()) {
    sendErrorResponse(request, 503, "L'horloge système n'est pas synchronisée (La validation TLS nécessite un serveur NTP)");
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

static void handleOtaUpdate(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (!index) {
    systemLogger.info("Début mise à jour OTA: " + filename);

    // Déterminer le type de mise à jour
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
    // Priorité 2: Détecter par le nom de fichier si pas de paramètre
    else if (filename.endsWith(".littlefs.bin") || filename.endsWith(".spiffs.bin") || filename.endsWith(".fs.bin")) {
      cmd = U_SPIFFS;
      systemLogger.info("Type de mise à jour: Filesystem (détection nom fichier)");
    } else {
      systemLogger.info("Type de mise à jour: Firmware (détection nom fichier)");
    }

    // Démarrer la mise à jour
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      Update.printError(Serial);
      systemLogger.error("Erreur démarrage OTA: " + String(Update.errorString()));
    }
  }

  // Écrire les données
  if (len) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      systemLogger.error("Erreur écriture OTA");
    } else {
      // Log progression toutes les 100KB
      static size_t lastLog = 0;
      if (index - lastLog >= 102400) {
        unsigned int percent = (index + len) * 100 / Update.size();
        systemLogger.info("Progression OTA: " + String(percent) + "%");
        lastLog = index;
      }
    }
  }

  // Finaliser
  if (final) {
    if (Update.end(true)) {
      systemLogger.info("Mise à jour OTA réussie. Redémarrage...");
    } else {
      Update.printError(Serial);
      systemLogger.error("Erreur finalisation OTA: " + String(Update.errorString()));
    }
  }
}

static void handleCheckUpdate(AsyncWebServerRequest* request) {
  systemLogger.info("Vérification des mises à jour GitHub...");

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

  int httpCode = https.GET();

  if (httpCode != HTTP_CODE_OK) {
    systemLogger.error("Erreur HTTP GitHub: " + String(httpCode));
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

    // Autres erreurs HTTP
    String errorMsg = "{\"error\":\"GitHub API error\",\"code\":" + String(httpCode) + "}";
    request->send(500, "application/json", errorMsg);
    return;
  }

  String payload = https.getString();
  https.end();

  // Parser la réponse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    systemLogger.error("Erreur parsing JSON GitHub");
    sendErrorResponse(request, 500, "JSON parse error");
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

  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = asset["name"].as<String>();
    if (name == "firmware.bin") {
      firmwareUrl = asset["browser_download_url"].as<String>();
    } else if (name == "littlefs.bin") {
      filesystemUrl = asset["browser_download_url"].as<String>();
    }
  }

  // Construire la réponse
  JsonDocument response;
  response["current_version"] = currentVersion;
  response["latest_version"] = latestVersion;
  response["update_available"] = updateAvailable;
  response["firmware_url"] = firmwareUrl;
  response["filesystem_url"] = filesystemUrl;
  response["release_notes"] = doc["body"];

  String json;
  serializeJson(response, json);

  systemLogger.info("Version actuelle: " + currentVersion + ", Dernière version: " + latestVersion);
  request->send(200, "application/json", json);
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

  // Paramètre optionnel pour contrôler le redémarrage
  bool shouldRestart = true;
  if (request->hasParam("restart", true)) {
    String restartParam = request->getParam("restart", true)->value();
    shouldRestart = (restartParam == "true" || restartParam == "1");
  }

  // Déterminer le type de mise à jour en fonction de l'URL
  int cmd = U_FLASH; // Par défaut: firmware
  if (url.indexOf("littlefs") >= 0 || url.indexOf("filesystem") >= 0) {
    cmd = U_SPIFFS;
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

  // Démarrer la mise à jour OTA
  if (!Update.begin(contentLength, cmd)) {
    systemLogger.error("Erreur démarrage OTA: " + String(Update.errorString()));
    http.end();
    sendErrorResponse(request, 500, "OTA begin failed");
    return;
  }

  // Lire et écrire les données par blocs
  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buff[512];

  while (http.connected() && (written < contentLength)) {
    size_t available = stream->available();

    if (available) {
      int c = stream->readBytes(buff, min(available, sizeof(buff)));

      if (c > 0) {
        if (Update.write(buff, c) != c) {
          systemLogger.error("Erreur écriture OTA");
          Update.abort();
          http.end();
          sendErrorResponse(request, 500, "OTA write failed");
          return;
        }
        written += c;

        // Log de progression tous les 100KB
        if (written % 102400 == 0 || written == contentLength) {
          unsigned int percent = (written * 100) / contentLength;
          systemLogger.info("Téléchargement: " + String(percent) + "%");
        }
      }
    }
    delay(kOtaYieldDelayMs);
  }

  http.end();

  // Finaliser la mise à jour
  if (Update.end(true)) {
    if (shouldRestart && g_restartRequested != nullptr && g_restartRequestedTime != nullptr) {
      systemLogger.info("Mise à jour GitHub réussie! Redémarrage...");
      request->send(200, "application/json", "{\"status\":\"success\"}");
      // Planifier le redémarrage dans update() pour ne pas bloquer
      *g_restartRequested = true;
      *g_restartRequestedTime = millis();
    } else {
      systemLogger.info("Mise à jour GitHub réussie (sans redémarrage)");
      request->send(200, "application/json", "{\"status\":\"success\"}");
    }
  } else {
    systemLogger.error("Erreur finalisation OTA: " + String(Update.errorString()));
    sendErrorResponse(request, 500, "OTA finalization failed");
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

      bool success = !Update.hasError();
      AsyncWebServerResponse *response = req->beginResponse(200, "text/plain", success ? "OK" : "FAIL");
      response->addHeader("Connection", "close");
      req->send(response);
      if (success && g_restartRequested != nullptr && g_restartRequestedTime != nullptr) {
        // Planifier le redémarrage dans update() pour ne pas bloquer
        *g_restartRequested = true;
        *g_restartRequestedTime = millis();
      }
    },
    handleOtaUpdate
  );
}
