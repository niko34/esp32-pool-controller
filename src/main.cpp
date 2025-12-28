#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPAsyncWiFiManager.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "constants.h"
#include "logger.h"
#include "auth.h"
#include "sensors.h"
#include "pump_controller.h"
#include "filtration.h"
#include "mqtt_manager.h"
#include "web_server.h"
#include "history.h"
#include "version.h"

// Variables globales
DNSServer dns;
AsyncWebServer httpServer(kHttpServerPort);
unsigned long lastMqttPublish = 0;
wifi_mode_t currentWifiMode = WIFI_MODE_NULL;

// Déclaration des fonctions
bool setupWiFi();
void applyTimeConfig();
void checkSystemHealth();

void setup() {
  Serial.begin(115200);
  delay(kSerialInitDelayMs);

  systemLogger.info("=== Démarrage ESP32 Pool Controller v" + String(FIRMWARE_VERSION) + " ===");
  systemLogger.info("Build: " + String(FIRMWARE_BUILD_DATE) + " " + String(FIRMWARE_BUILD_TIME));

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
  history.begin();

  // Initialisation relais éclairage
  pinMode(LIGHTING_RELAY_PIN, OUTPUT);

  // Appliquer l'état initial de l'éclairage
  if (lightingCfg.enabled) {
    digitalWrite(LIGHTING_RELAY_PIN, HIGH);
    systemLogger.info("Éclairage activé au démarrage");
  } else {
    digitalWrite(LIGHTING_RELAY_PIN, LOW);
    systemLogger.info("Éclairage désactivé au démarrage");
  }

  // Configuration WiFi
  if (setupWiFi()) {
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

    // Serveur Web (partager le serveur avec WiFiManager)
    webServer.begin(&httpServer, &dns);

    // Connexion MQTT initiale
    if (mqttCfg.enabled) {
      mqttManager.requestReconnect();
    }
  }

  systemLogger.info("Initialisation terminée");
  esp_task_wdt_reset(); // Reset watchdog après init
}

void loop() {
  // Reset watchdog au début de chaque cycle
  esp_task_wdt_reset();

  unsigned long now = millis();
  currentWifiMode = WiFi.getMode();

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

bool setupWiFi() {
  WiFi.mode(WIFI_STA);
  currentWifiMode = WiFi.getMode();
  AsyncWiFiManager wm(&httpServer, &dns);

  systemLogger.info("Tentative connexion WiFi...");

  if (!wm.autoConnect("PoolControllerAP", "12345678")) {
    systemLogger.error("Échec connexion WiFi");
    currentWifiMode = WiFi.getMode();
    return false;
  }

  systemLogger.info("WiFi connecté: " + WiFi.SSID());
  systemLogger.info("IP: " + WiFi.localIP().toString());
  currentWifiMode = WiFi.getMode();
  return true;
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

  // Vérifier connexion WiFi
  if (!WiFi.isConnected()) {
    systemLogger.warning("WiFi déconnecté, tentative de reconnexion");
    WiFi.reconnect();
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
