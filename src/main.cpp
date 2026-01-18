#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <nvs_flash.h>

#include "config.h"
#include "constants.h"
#include "logger.h"
#include "auth.h"
#include "sensors.h"
#include "pump_controller.h"
#include "filtration.h"
#include "lighting.h"
#include "mqtt_manager.h"
#include "web_server.h"
#include "web_routes_config.h"
#include "history.h"
#include "version.h"

// Variables globales
DNSServer dns;
AsyncWebServer httpServer(kHttpServerPort);
unsigned long lastMqttPublish = 0;
wifi_mode_t currentWifiMode = WIFI_MODE_NULL;

// Déclaration des fonctions
bool setupWiFi();
void resetWiFiSettings();
void applyTimeConfig();
void checkSystemHealth();
void checkPasswordResetButton();

void setup() {
  Serial.begin(115200);
  delay(kSerialInitDelayMs);

  systemLogger.info("=== Démarrage ESP32 Pool Controller v" + String(FIRMWARE_VERSION) + " ===");
  systemLogger.info("Build: " + String(FIRMWARE_BUILD_DATE) + " " + String(FIRMWARE_BUILD_TIME));

  // Vérifier le bouton de réinitialisation AVANT de charger la config
  checkPasswordResetButton();

  // Initialisation watchdog
  esp_task_wdt_init(kWatchdogTimeoutSec, true);
  esp_task_wdt_add(NULL);
  systemLogger.info("Watchdog activé (" + String(kWatchdogTimeoutSec) + "s)");

  // Montage système de fichiers
  if (!LittleFS.begin(true)) {
    systemLogger.critical("Échec montage LittleFS !");
  } else {
    systemLogger.info("LittleFS monté avec succès");
  }

  // Initialisation des mutex de protection concurrence
  initConfigMutexes();

  // Chargement configuration
  loadMqttConfig();

  // Initialisation authentification (après chargement config)
  authManager.setEnabled(authCfg.enabled);
  authManager.setPassword(authCfg.adminPassword);
  authManager.setApiToken(authCfg.apiToken);
  authManager.begin();

  // Sauvegarder le token généré si nécessaire
  if (authCfg.apiToken != authManager.getApiToken()) {
    authCfg.apiToken = authManager.getApiToken();
    saveMqttConfig();
  }

  // Initialisation des modules
  sensors.begin();
  PumpController.begin();
  filtration.begin();
  lighting.begin();
  history.begin();

  // Configuration WiFi
  bool wifiConnected = setupWiFi();

  if (wifiConnected) {
    // mDNS
    if (!MDNS.begin(kMdnsHostname)) {
      systemLogger.error("Échec démarrage mDNS");
    } else {
      MDNS.addService("http", "tcp", kMdnsHttpPort);
      systemLogger.info("mDNS: poolcontroller.local disponible");
    }

    // Initialisation MQTT
    mqttManager.begin();
    applyTimeConfig();

    // Connexion MQTT initiale
    if (mqttCfg.enabled) {
      mqttManager.requestReconnect();
    }
  }

  // Serveur Web (disponible en STA ou AP)
  webServer.begin(&httpServer, &dns);

  systemLogger.info("Initialisation terminée");
  esp_task_wdt_reset(); // Reset watchdog après init
}

void loop() {
  // Reset watchdog au début de chaque cycle
  esp_task_wdt_reset();

  unsigned long now = millis();
  currentWifiMode = WiFi.getMode();

  // Traiter les reconnexions WiFi asynchrones si nécessaire
  processWifiReconnectIfNeeded();

  // Mise à jour des gestionnaires
  webServer.update();
  mqttManager.update();
  history.update();

  // Lecture capteurs à chaque loop (les capteurs gèrent leur propre throttling interne)
  // DS18B20 : machine à états non-bloquante (request → wait → read) toutes les 2s
  // pH/ORP : lecture limitée à toutes les 5s en interne
  sensors.update();

  // Publication MQTT périodique
  if (mqttManager.isConnected() && now - lastMqttPublish >= kMqttPublishIntervalMs) {
    mqttManager.publishAllStates();
    lastMqttPublish = now;
  }

  // Contrôle filtration (s'exécute fréquemment pour précision)
  filtration.update();

  // Contrôle éclairage
  lighting.update();

  // Contrôle pompes dosage
  PumpController.update();

  // Vérification santé système périodique
  static unsigned long lastHealthCheck = 0;
  if (now - lastHealthCheck >= kHealthCheckIntervalMs) {
    checkSystemHealth();
    lastHealthCheck = now;
    esp_task_wdt_reset();
  }

  // Publication diagnostic MQTT périodique
  static unsigned long lastDiagnosticPublish = 0;
  if (mqttManager.isConnected() && now - lastDiagnosticPublish >= kDiagnosticPublishIntervalMs) {
    mqttManager.publishDiagnostic();
    lastDiagnosticPublish = now;
  }

  // Petit délai pour ne pas monopoliser le CPU
  delay(kLoopDelayMs);
}

// Fonction utilitaire pour convertir le statut WiFi en string lisible
String getWifiStatusString(int status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAILABLE";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

bool setupWiFi() {
  WiFi.mode(WIFI_STA);
  currentWifiMode = WiFi.getMode();

  // Afficher les credentials WiFi stockés en NVS (pour debug)
  String storedSsid = WiFi.SSID();
  String storedPassword = WiFi.psk();
  systemLogger.info("=== DEBUG WiFi Credentials ===");
  systemLogger.info("SSID stocké: '" + storedSsid + "' (longueur: " + String(storedSsid.length()) + ")");
  systemLogger.info("Password stocké: '" + storedPassword + "' (longueur: " + String(storedPassword.length()) + ")");
  systemLogger.info("==============================");

  systemLogger.info("Tentative connexion WiFi...");

  // Connexion avec credentials sauvegardés (NVS)
  WiFi.begin();
  unsigned long start = millis();
  int lastStatus = -1;
  while (WiFi.status() != WL_CONNECTED && millis() - start < kWifiConnectTimeoutMs) {
    int currentStatus = WiFi.status();
    if (currentStatus != lastStatus) {
      // Afficher le code de statut WiFi
      systemLogger.info("Statut WiFi: " + String(currentStatus) + " (" + getWifiStatusString(currentStatus) + ")");
      lastStatus = currentStatus;
    }
    delay(250);
  }

  // Afficher le statut final
  int finalStatus = WiFi.status();
  systemLogger.info("Statut final: " + String(finalStatus) + " (" + getWifiStatusString(finalStatus) + ")");

  auto startApMode = [](bool keepSta) {
    // Si on passe en mode AP seul (échec connexion), désactiver la persistence
    // pour éviter que le changement de mode n'efface les credentials WiFi stockés
    if (!keepSta) {
      WiFi.persistent(false);
    }
    WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
    bool apStarted = WiFi.softAP("PoolControllerAP", "12345678");
    if (apStarted) {
      systemLogger.info("AP démarré: PoolControllerAP");
      systemLogger.info("IP AP: " + WiFi.softAPIP().toString());
      dns.start(53, "*", WiFi.softAPIP());
    } else {
      systemLogger.error("Impossible de démarrer le mode AP");
    }
  };

  if (WiFi.status() == WL_CONNECTED) {
    systemLogger.info("WiFi connecté: " + WiFi.SSID());
    systemLogger.info("IP: " + WiFi.localIP().toString());
    currentWifiMode = WiFi.getMode();

    // Réinitialiser le flag disableApOnBoot après une connexion réussie
    if (authCfg.disableApOnBoot) {
      systemLogger.info("Flag disableApOnBoot réinitialisé (connexion WiFi réussie)");
      authCfg.disableApOnBoot = false;
      saveMqttConfig();
    }

    // Activer le mode AP seulement si :
    // - forceWifiConfig est activé OU
    // - premier démarrage détecté ET
    // - le flag disableApOnBoot n'est PAS activé
    if ((authCfg.forceWifiConfig || authManager.isFirstBootDetected()) && !authCfg.disableApOnBoot) {
      systemLogger.warning("Mode AP activé (reset password ou premier démarrage): activation AP + STA");
      startApMode(true);
    } else if (authCfg.disableApOnBoot) {
      systemLogger.info("Mode AP désactivé (flag disableApOnBoot actif) - Mode STA uniquement");
    }
    return true;
  }

  // Échec de connexion WiFi
  // Ne pas démarrer l'AP si le flag disableApOnBoot est activé
  // (peut arriver si les credentials WiFi sont incorrects après configuration)
  if (authCfg.disableApOnBoot) {
    systemLogger.warning("Échec connexion WiFi mais disableApOnBoot actif - Mode STA sans AP");
    systemLogger.warning("L'ESP32 restera sans AP. Réinitialisez le mot de passe pour activer l'AP.");
    // Réinitialiser le flag après plusieurs échecs pour éviter un blocage permanent
    authCfg.disableApOnBoot = false;
    saveMqttConfig();
    return false;
  }

  systemLogger.error("Échec connexion WiFi, activation du mode AP");
  startApMode(false);
  currentWifiMode = WiFi.getMode();
  return false;
}

void resetWiFiSettings() {
  systemLogger.warning("Effacement complet de la partition NVS (factory reset)...");

  // Déconnecter le WiFi avant d'effacer
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Effacer TOUTE la partition NVS - Équivalent à un factory reset
  // Cela efface TOUTES les données stockées : WiFi, Preferences, etc.
  esp_err_t err = nvs_flash_erase();
  if (err == ESP_OK) {
    systemLogger.info("Partition NVS effacée avec succès");

    // Réinitialiser la partition NVS
    err = nvs_flash_init();
    if (err == ESP_OK) {
      systemLogger.info("Partition NVS réinitialisée");
    } else {
      systemLogger.error("Erreur réinitialisation NVS: " + String(err));
    }
  } else {
    systemLogger.error("Erreur effacement NVS: " + String(err));
  }

  systemLogger.info("Factory reset complet - Redémarrage nécessaire");
}

void applyTimeConfig() {
  ensureTimezoneValid();
  applyTimezoneEnv();

  if (mqttCfg.timeUseNtp) {
    if (WiFi.isConnected()) {
      const TimezoneInfo* tz = currentTimezone();
      if (mqttCfg.ntpServer.length() == 0) {
        mqttCfg.ntpServer = "pool.ntp.org";
      }
      configTzTime(tz->posix, mqttCfg.ntpServer.c_str(), "time.nist.gov", "pool.ntp.org");
      systemLogger.info("Synchronisation NTP demandée: " + mqttCfg.ntpServer);
    } else {
      systemLogger.warning("NTP activé mais WiFi indisponible");
    }
  }
}

void checkSystemHealth() {
  // Vérifier l'état général du système
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < kMinFreeHeapBytes) {
    systemLogger.critical("Mémoire faible: " + String(freeHeap) + " bytes");
    mqttManager.publishAlert("low_memory", "Free heap: " + String(freeHeap) + " bytes");
  }

  // Vérifier connexion WiFi et activer AP en secours si nécessaire
  static unsigned long lastWifiCheckTime = 0;
  static int wifiReconnectAttempts = 0;
  wifi_mode_t mode = WiFi.getMode();

  if (!WiFi.isConnected() && (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)) {
    unsigned long now = millis();

    // Première tentative ou si 30 secondes se sont écoulées depuis la dernière tentative
    if (lastWifiCheckTime == 0 || (now - lastWifiCheckTime >= 30000)) {
      systemLogger.warning("WiFi déconnecté, tentative de reconnexion (" + String(wifiReconnectAttempts + 1) + "/3)");
      WiFi.reconnect();
      lastWifiCheckTime = now;
      wifiReconnectAttempts++;

      // Après 3 tentatives infructueuses, activer le mode AP en secours
      if (wifiReconnectAttempts >= 3 && mode == WIFI_MODE_STA) {
        systemLogger.error("Impossible de reconnecter le WiFi après 3 tentatives, activation du mode AP");
        WiFi.mode(WIFI_AP_STA);
        bool apStarted = WiFi.softAP("PoolControllerAP", "12345678");
        if (apStarted) {
          systemLogger.info("AP démarré en mode secours: PoolControllerAP");
          systemLogger.info("IP AP: " + WiFi.softAPIP().toString());
          dns.start(53, "*", WiFi.softAPIP());
        }
        wifiReconnectAttempts = 0; // Réinitialiser pour permettre de nouvelles tentatives plus tard
      }
    }
  } else if (WiFi.isConnected()) {
    // Réinitialiser le compteur si la connexion est rétablie
    if (wifiReconnectAttempts > 0) {
      systemLogger.info("WiFi reconnecté avec succès");
      wifiReconnectAttempts = 0;
      lastWifiCheckTime = 0;
    }
  }

  // Vérifier connexion MQTT
  if (mqttCfg.enabled && !mqttManager.isConnected()) {
    systemLogger.warning("MQTT déconnecté, reconnexion automatique");
    mqttManager.requestReconnect();
  }

  // Vérifier limites de sécurité
  if (safetyLimits.phLimitReached) {
    mqttManager.publishAlert("ph_limit", "Limite journalière pH- atteinte");
  }
  if (safetyLimits.orpLimitReached) {
    mqttManager.publishAlert("orp_limit", "Limite journalière chlore atteinte");
  }

  // Vérifier valeurs capteurs aberrantes
  float ph = sensors.getPh();
  float orp = sensors.getOrp();
  float temp = sensors.getTemperature();

  if (ph < 5.0f || ph > 9.0f) {
    systemLogger.warning("Valeur pH anormale: " + String(ph));
    mqttManager.publishAlert("ph_abnormal", "pH=" + String(ph));
  }
  if (orp < 400.0f || orp > 900.0f) {
    systemLogger.warning("Valeur ORP anormale: " + String(orp));
    mqttManager.publishAlert("orp_abnormal", "ORP=" + String(orp));
  }
  if (!isnan(temp) && (temp < 5.0f || temp > 40.0f)) {
    systemLogger.warning("Température anormale: " + String(temp));
    mqttManager.publishAlert("temp_abnormal", "Temp=" + String(temp) + "°C");
  }

  systemLogger.debug("Health check OK - Heap: " + String(freeHeap) + " bytes");
}

void checkPasswordResetButton() {
  // Initialiser le bouton de réinitialisation (GPIO4) et la LED
  pinMode(PASSWORD_RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUILTIN_LED_PIN, OUTPUT);
  digitalWrite(BUILTIN_LED_PIN, LOW);  // LED éteinte par défaut

  // Vérifier si le bouton est maintenu enfoncé (actif bas)
  if (digitalRead(PASSWORD_RESET_BUTTON_PIN) == HIGH) {
    // Bouton relâché, pas de réinitialisation
    return;
  }

  systemLogger.warning("Bouton de réinitialisation détecté enfoncé au démarrage");
  systemLogger.info("Maintenez enfoncé pendant 10s pour réinitialiser le mot de passe...");

  unsigned long startTime = millis();
  bool resetConfirmed = true;

  // Faire clignoter la LED pendant 10 secondes
  while (millis() - startTime < kPasswordResetButtonHoldMs) {
    // Vérifier que le bouton est toujours enfoncé
    if (digitalRead(PASSWORD_RESET_BUTTON_PIN) == HIGH) {
      resetConfirmed = false;
      systemLogger.info("Bouton relâché - Réinitialisation annulée");
      break;
    }

    // Clignoter la LED (100ms ON / 100ms OFF)
    digitalWrite(BUILTIN_LED_PIN, (millis() / 100) % 2);
    delay(50);
  }

  // Éteindre la LED
  digitalWrite(BUILTIN_LED_PIN, LOW);

  if (resetConfirmed) {
    systemLogger.critical("=== RÉINITIALISATION MOT DE PASSE CONFIRMÉE ===");

    // Charger la config actuelle
    loadMqttConfig();

    // Réinitialiser le mot de passe
    authCfg.adminPassword = "admin";
    authCfg.apiToken = "";
    authCfg.forceWifiConfig = true;

    // Sauvegarder la config avec le nouveau mot de passe
    saveMqttConfig();

    systemLogger.critical("Mot de passe réinitialisé à 'admin'");
    systemLogger.warning("Changement de mot de passe obligatoire au prochain login");

    // Effacer les credentials WiFi
    systemLogger.warning("Effacement des credentials WiFi...");
    resetWiFiSettings();
    systemLogger.info("WiFi réinitialisé - Mode AP uniquement au prochain démarrage");

    // Faire clignoter rapidement la LED 5 fois pour confirmer
    for (int i = 0; i < 10; i++) {
      digitalWrite(BUILTIN_LED_PIN, i % 2);
      delay(200);
    }

    // Redémarrer l'ESP32 pour appliquer les changements
    systemLogger.critical("Redémarrage de l'ESP32...");
    delay(1000);
    ESP.restart();
  }
}
