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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <time.h>
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
static bool g_uploadDigestInvalid = false;      // Param sha256 fourni mais malformé → refus au final
static AsyncWebServerRequest* g_uploadOwner = nullptr;  // Requête propriétaire de la session d'upload

// Marque une requête comme rejetée (upload concurrent) via _tempObject :
// marqueur par requête, libéré automatiquement par free() dans le destructeur
// d'AsyncWebServerRequest — pas de fuite même si le client déconnecte.
static void markUploadRejected(AsyncWebServerRequest* request) {
  if (request->_tempObject == nullptr) {
    request->_tempObject = malloc(1);
  }
}

static void handleOtaUpdate(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
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
        PumpController.setOtaInProgress(false);
        g_uploadOwner = nullptr;
      }
    });

    systemLogger.info("Début mise à jour OTA: " + filename);
    PumpController.setOtaInProgress(true);

    // Réinitialiser TOUT l'état statique de l'upload (AC5 : deux tentatives
    // successives sans reboot ne doivent partager aucun état).
    g_uploadLastLog = 0;
    g_uploadHasExpected = false;
    g_uploadDigestInvalid = false;
    g_uploadHasher.begin();

    // Param POST `sha256` OPTIONNEL (upload manuel : l'utilisateur est
    // physiquement présent). Accepté avec ou sans préfixe "sha256:".
    // Présent mais malformé → refus explicite au final (fail-closed).
    if (request->hasParam("sha256", true)) {
      String provided = request->getParam("sha256", true)->value();
      if (parseSha256Digest(provided.c_str(), g_uploadExpected)) {
        g_uploadHasExpected = true;
      } else {
        String prefixed = String(kOtaSha256Prefix) + provided;
        if (parseSha256Digest(prefixed.c_str(), g_uploadExpected)) {
          g_uploadHasExpected = true;
        } else {
          g_uploadDigestInvalid = true;
          systemLogger.critical("Intégrité OTA: empreinte sha256 fournie invalide — le flash sera refusé");
        }
      }
    }

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

    // Démonter LittleFS avant la mise à jour du filesystem
    if (cmd == U_SPIFFS) {
      LittleFS.end();
      systemLogger.info("LittleFS démonté pour mise à jour manuelle");
    }

    // Démarrer la mise à jour
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      Update.printError(Serial);
      systemLogger.error("Erreur démarrage OTA: " + String(Update.errorString()));
      // Chemin terminal : réarmer le dosage (sinon pompes inhibées jusqu'au
      // reboot — le final est sauté par la garde owner) et libérer le verrou.
      PumpController.setOtaInProgress(false);
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
      Update.printError(Serial);
      systemLogger.error("Erreur écriture OTA");
      Update.abort();
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

    // Param sha256 fourni mais malformé → refus explicite (fail-closed).
    // Update.abort() garantit hasError() == true → réponse "FAIL" au client.
    if (g_uploadDigestInvalid) {
      systemLogger.critical("Intégrité OTA: flash refusé (empreinte sha256 fournie invalide)");
      Update.abort();
      PumpController.setOtaInProgress(false);
      g_uploadOwner = nullptr;  // Chemin terminal : libérer le verrou
      return;
    }

    // Param sha256 fourni et valide → comparaison AVANT Update.end() :
    // en cas de mismatch l'image n'est jamais activée.
    if (g_uploadHasExpected && !sha256Equal(computed, g_uploadExpected)) {
      char expectedHex[kOtaSha256HexLen + 1];
      sha256ToHex(g_uploadExpected, expectedHex, sizeof(expectedHex));
      systemLogger.critical("Intégrité OTA: empreinte SHA-256 non conforme (attendue " +
                            String(expectedHex) + ", calculée " + String(computedHex) +
                            ") — flash refusé");
      Update.abort();
      PumpController.setOtaInProgress(false);
      g_uploadOwner = nullptr;  // Chemin terminal : libérer le verrou
      return;
    }

    if (Update.end(true)) {
      systemLogger.info("Mise à jour OTA réussie. Redémarrage...");
      if (g_restartRequested == nullptr || g_restartRequestedTime == nullptr) {
        PumpController.setOtaInProgress(false);
      }
    } else {
      Update.printError(Serial);
      systemLogger.error("Erreur finalisation OTA: " + String(Update.errorString()));
      PumpController.setOtaInProgress(false);
    }
    g_uploadOwner = nullptr;  // Chemin terminal (succès ou échec) : libérer le verrou
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
  // Empreintes SHA-256 des assets (champ `digest` de l'API GitHub, format
  // "sha256:<64hex>"). Chaîne vide si absent — le refus fail-closed se fait
  // au moment du téléchargement (/download-update), pas ici.
  String firmwareDigest = "";
  String filesystemDigest = "";

  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = asset["name"].as<String>();
    if (name == "firmware.bin") {
      firmwareUrl = asset["browser_download_url"].as<String>();
      firmwareDigest = asset["digest"] | "";
    } else if (name == "littlefs.bin") {
      filesystemUrl = asset["browser_download_url"].as<String>();
      filesystemDigest = asset["digest"] | "";
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
  response["release_name"] = doc["name"];
  response["published_at"] = doc["published_at"];
  response["release_notes"] = doc["body"];

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

  // Paramètre optionnel pour contrôler le redémarrage
  bool shouldRestart = true;
  if (request->hasParam("restart", true)) {
    String restartParam = request->getParam("restart", true)->value();
    shouldRestart = (restartParam == "true" || restartParam == "1");
  }

  // Déterminer le type de mise à jour en fonction de l'URL
  bool isFilesystem = (url.indexOf("littlefs") >= 0 || url.indexOf("filesystem") >= 0);

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
    if (!sha256Equal(computedFs, expectedHash)) {
      char expectedHex[kOtaSha256HexLen + 1];
      char computedHex[kOtaSha256HexLen + 1];
      sha256ToHex(expectedHash, expectedHex, sizeof(expectedHex));
      sha256ToHex(computedFs, computedHex, sizeof(computedHex));
      systemLogger.critical("Intégrité OTA FS: empreinte SHA-256 non conforme (attendue " +
                            String(expectedHex) + ", calculée " + String(computedHex) +
                            ") — redémarrage annulé");
      // Remontage best-effort : la partition contient une image corrompue,
      // LittleFS échouera probablement (l'UI restera servie depuis le cache
      // navigateur jusqu'à un nouvel OTA FS réussi).
      if (!LittleFS.begin(false)) {
        systemLogger.warning("Impossible de remonter LittleFS après échec d'intégrité");
      }
      PumpController.setOtaInProgress(false);  // Appariement du setOtaInProgress(true)
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
            systemLogger.error("Erreur écriture OTA");
            Update.abort();
            http.end();
            PumpController.setOtaInProgress(false);
            sendErrorResponse(request, 500, "OTA write failed");
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

      // Requête rejetée pour upload concurrent (marqueur posé par
      // markUploadRejected) : réponse d'erreur dédiée — Update.hasError()
      // reflète la session de l'AUTRE upload et donnerait un faux "OK".
      if (req->_tempObject != nullptr) {
        AsyncWebServerResponse *response = req->beginResponse(409, "text/plain",
          "Upload refusé: une mise à jour OTA est déjà en cours");
        response->addHeader("Connection", "close");
        req->send(response);
        return;
      }

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
