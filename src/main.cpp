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
#include "web_routes_control.h"
#include "history.h"
#include "version.h"
#include "rtc_manager.h"
#include "uart_transport.h"
#include "uart_protocol.h"

// Variables globales
DNSServer dns;
AsyncWebServer httpServer(kHttpServerPort);
unsigned long lastMqttPublish = 0;
wifi_mode_t currentWifiMode = WIFI_MODE_NULL;
bool ntpSyncedOnce = false;  // Flag pour éviter sync RTC multiple

// Déclaration des fonctions
bool setupWiFi();
void resetWiFiSettings();
void applyTimeConfig();
void checkSystemHealth();
void onNtpTimeSync();

void setup() {
  Serial.begin(115200);
  delay(kSerialInitDelayMs);

  systemLogger.begin();  // Initialise le mutex avant tout accès multi-core
  systemLogger.info("=== Démarrage ESP32 Pool Controller v" + String(FIRMWARE_VERSION) + " ===");
  systemLogger.info("Build: " + String(FIRMWARE_BUILD_DATE) + " " + String(FIRMWARE_BUILD_TIME));

  // Initialisation GPIO bouton factory reset et LED intégrée
  pinMode(FACTORY_RESET_BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(BUILTIN_LED_PIN, OUTPUT);
  digitalWrite(BUILTIN_LED_PIN, LOW);

  // Initialisation watchdog
  esp_task_wdt_init(kWatchdogTimeoutSec, true);
  esp_task_wdt_add(NULL);
  systemLogger.info("Watchdog activé (" + String(kWatchdogTimeoutSec) + "s)");

  // Montage système de fichiers
  if (!LittleFS.begin(false)) {
    systemLogger.critical("Échec montage LittleFS !");
  } else {
    systemLogger.info("LittleFS monté avec succès");
  }

  // Initialisation des mutex de protection concurrence
  initConfigMutexes();

  // Chargement configuration
  loadMqttConfig();
  loadProductConfig();

  // Initialisation UART écran (après chargement config pour respecter screenEnabled)
  if (authCfg.screenEnabled) {
    uartTransport.begin();
    systemLogger.info("Écran LVGL activé — UART2 initialisé");
  }

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
  sensors.begin();  // Initialise aussi Wire (I2C)

  // Initialisation RTC DS1307 (après sensors.begin() qui initialise I2C)
  if (rtcManager.begin()) {
    // Si le RTC a une heure valide, l'appliquer au système
    // Cela donne une heure approximative avant que NTP ne soit disponible
    if (rtcManager.isTimeValid()) {
      rtcManager.applyToSystem();
    } else if (rtcManager.hasLostPower()) {
      systemLogger.warning("RTC: Heure non valide (batterie vide?), attente sync NTP ou réglage manuel");
    }
  }

  PumpController.begin();
  // En mode continu : armer le timer de stabilisation dès le boot
  if (mqttCfg.regulationMode == "continu") {
    PumpController.armStabilizationTimer();
  }
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

    // Connexion MQTT initiale gérée par mqttTask au 1er tour de taskLoop() — voir ADR-0011.
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
  mqttManager.update();              // No-op depuis ADR-0011 (mqttTask gère tout)
  mqttManager.drainCommandQueue();   // Applique les commandes HA reçues sous configMutex
  history.update();
  systemLogger.update();
  updateManualInject();
  if (authCfg.screenEnabled) uartTransport.update();

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

  // Détection appui long bouton factory reset (10s) en cours de fonctionnement
  {
    static unsigned long buttonPressStart = 0;
    static bool lastButtonState = LOW;
    static bool ledState = false;
    static unsigned long lastLedToggle = 0;
    bool buttonState = digitalRead(FACTORY_RESET_BUTTON_PIN);

    if (buttonState == HIGH && lastButtonState == LOW) {
      // Début d'appui
      buttonPressStart = now;
      systemLogger.info("Bouton reset enfoncé - maintenir 10s pour factory reset");
    } else if (buttonState == HIGH && buttonPressStart > 0) {
      unsigned long held = now - buttonPressStart;

      // Faire clignoter la LED pendant l'appui
      if (now - lastLedToggle >= 200) {
        ledState = !ledState;
        digitalWrite(BUILTIN_LED_PIN, ledState);
        lastLedToggle = now;
        esp_task_wdt_reset();
      }

      // Factory reset après 10s
      if (held >= kFactoryResetButtonHoldMs) {
        systemLogger.critical("=== FACTORY RESET CONFIRMÉ (appui 10s) ===");
        // Clignotement rapide de confirmation (5 fois)
        for (int i = 0; i < 10; i++) {
          digitalWrite(BUILTIN_LED_PIN, i % 2);
          delay(150);
        }
        digitalWrite(BUILTIN_LED_PIN, LOW);
        resetWiFiSettings();
        delay(500);
        mqttManager.shutdownForRestart();  // ADR-0011 : flush status=offline + stop mqttTask
        ESP.restart();
      }
    } else if (buttonState == LOW && lastButtonState == HIGH) {
      // Relâché avant 10s
      if (buttonPressStart > 0 && (now - buttonPressStart) < kFactoryResetButtonHoldMs) {
        systemLogger.info("Bouton relâché - factory reset annulé");
      }
      buttonPressStart = 0;
      digitalWrite(BUILTIN_LED_PIN, LOW);
    }

    lastButtonState = buttonState;
  }

  // Vérification santé système périodique
  static unsigned long lastHealthCheck = 0;
  if (now - lastHealthCheck >= kHealthCheckIntervalMs) {
    checkSystemHealth();
    lastHealthCheck = now;
    esp_task_wdt_reset();
  }

  // Vérification sync NTP et mise à jour RTC
  if (!ntpSyncedOnce && mqttCfg.timeUseNtp && WiFi.isConnected()) {
    time_t nowTime;
    time(&nowTime);
    // Si l'heure est valide (après 2021), NTP a synchronisé
    if (nowTime > 1609459200) {
      onNtpTimeSync();
    }
  }

  // Sauvegarde périodique des volumes de produits (toutes les 60s si modifié)
  static unsigned long lastProductSave = 0;
  if (productConfigDirty && now - lastProductSave >= 60000UL) {
    saveProductConfig();
    mqttManager.publishProductState();
    lastProductSave = now;
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
  // Désactiver le mode économie d'énergie : la radio reste allumée en permanence.
  // Sans ça, le mode par défaut WIFI_PS_MIN_MODEM fait dormir la radio entre les beacons DTIM,
  // ce qui cause une latence ping de 90-260ms au lieu de <10ms et des déconnexions MQTT toutes
  // les 1-2h (paquets queue côté AP qui finissent par déborder). L'ESP32 est alimenté en
  // permanence ici, le gain en autonomie n'a pas d'intérêt — la stabilité prime.
  WiFi.setSleep(false);

  // Logger les événements WiFi pour diagnostiquer les blackouts réseau (ping qui ne répond plus,
  // app injoignable, MQTT qui se reconnecte). Sans ces logs, impossible de savoir si la cause
  // est côté Wi-Fi (drop AP, RSSI, DHCP renew) ou côté firmware/broker.
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        systemLogger.warning("WiFi déconnecté (reason=" + String(reason) + ")");
        break;
      }
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        systemLogger.info("WiFi associé à l'AP");
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        systemLogger.info("WiFi IP obtenue: " + WiFi.localIP().toString() +
                          " RSSI=" + String(WiFi.RSSI()) + "dBm");
        break;
      case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        systemLogger.warning("WiFi IP perdue");
        break;
      default:
        break;
    }
  });

  currentWifiMode = WiFi.getMode();

  // Afficher les credentials WiFi stockés en NVS (pour debug)
  String storedSsid = WiFi.SSID();
  String storedPassword = WiFi.psk();
  //systemLogger.info("=== DEBUG WiFi Credentials ===");
  //systemLogger.info("SSID stocké: '" + storedSsid + "' (longueur: " + String(storedSsid.length()) + ")");
  //systemLogger.info("Password stocké: '" + storedPassword + "' (longueur: " + String(storedPassword.length()) + ")");
  //systemLogger.info("==============================");

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
    // Toujours utiliser WIFI_AP_STA pour préserver les credentials NVS.
    // WIFI_AP seul peut effacer les credentials sur certaines versions IDF.
    (void)keepSta;
    WiFi.mode(WIFI_AP_STA);
    bool apStarted = WiFi.softAP("PoolControllerAP", authManager.getApPassword().c_str());
    if (apStarted) {
      systemLogger.info("AP démarré: PoolControllerAP");
      // Logger le mot de passe en clair uniquement si le wizard n'est pas encore complété
      // (premier boot, étiquette pas encore écrite). Après setup, utiliser GET /auth/ap-password.
      if (authManager.isFirstBootDetected()) {
        systemLogger.info("AP Password: " + authManager.getApPassword());
      } else {
        systemLogger.info("AP Password: [voir etiquette ou GET /auth/ap-password]");
      }
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

  // Préserver le mot de passe AP : identifiant matériel unique, doit survivre au factory reset
  String savedApPassword = authCfg.apPassword;

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

      // Restaurer le mot de passe AP immédiatement pour qu'il survive au redémarrage
      if (!savedApPassword.isEmpty()) {
        Preferences prefs;
        if (prefs.begin("poolctrl", false)) {
          prefs.putString("auth_ap_pwd", savedApPassword);
          prefs.end();
          systemLogger.info("Mot de passe AP restauré après factory reset");
        }
      }
    } else {
      systemLogger.error("Erreur réinitialisation NVS: " + String(err));
    }
  } else {
    systemLogger.error("Erreur effacement NVS: " + String(err));
  }

  systemLogger.info("Factory reset complet - Redémarrage nécessaire");
}

// Callback appelé quand NTP se synchronise
void onNtpTimeSync() {
  if (!ntpSyncedOnce) {
    ntpSyncedOnce = true;
    systemLogger.info("NTP synchronisé avec succès");

    // Mettre à jour le RTC avec l'heure NTP
    if (rtcManager.isAvailable()) {
      if (rtcManager.syncFromSystem()) {
        systemLogger.info("RTC mis à jour depuis NTP");
      }
    }
  }
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

      // Reset le flag pour permettre une nouvelle sync RTC
      ntpSyncedOnce = false;
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
      WiFi.disconnect(false);  // Libère la stack WiFi sans effacer les credentials
      WiFi.reconnect();
      lastWifiCheckTime = now;
      wifiReconnectAttempts++;

      // Après 3 tentatives infructueuses, activer le mode AP en secours
      if (wifiReconnectAttempts >= 3 && mode == WIFI_MODE_STA) {
        systemLogger.error("Impossible de reconnecter le WiFi après 3 tentatives, activation du mode AP");
        WiFi.mode(WIFI_AP_STA);
        bool apStarted = WiFi.softAP("PoolControllerAP", authManager.getApPassword().c_str());
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
      // Redémarrer mDNS : le service ne survit pas à une déconnexion WiFi sur ESP32
      MDNS.end();
      if (MDNS.begin(kMdnsHostname)) {
        MDNS.addService("http", "tcp", kMdnsHttpPort);
        systemLogger.info("mDNS relancé: poolcontroller.local");
      } else {
        systemLogger.warning("Échec relance mDNS après reconnexion WiFi");
      }
    }
    // Désactiver l'AP secours si le WiFi est de nouveau connecté
    if (mode == WIFI_MODE_APSTA) {
      systemLogger.info("WiFi rétabli — désactivation du mode AP secours");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      currentWifiMode = WIFI_MODE_STA;
    }
  }

  // La reconnexion MQTT est gérée par MqttManager::update() (avec rate-limit + backoff exponentiel)
  // Ne PAS appeler requestReconnect() périodiquement : ça réinitialise le backoff et empêche
  // l'augmentation jusqu'à 120s en cas de broker injoignable.

  // Vérifier limites de sécurité
  static bool lastPhLimitReached = false;
  static bool lastOrpLimitReached = false;

  if (safetyLimits.phLimitReached) {
    mqttManager.publishAlert("ph_limit", "Limite journalière pH- atteinte");
    if (!lastPhLimitReached && authCfg.screenEnabled) {
      uartProtocol.sendAlarmRaised("PH_LIMIT", "Limite journalière pH atteinte");
    }
  } else if (lastPhLimitReached && authCfg.screenEnabled) {
    uartProtocol.sendAlarmCleared("PH_LIMIT");
  }
  lastPhLimitReached = safetyLimits.phLimitReached;

  if (safetyLimits.orpLimitReached) {
    mqttManager.publishAlert("orp_limit", "Limite journalière chlore atteinte");
    if (!lastOrpLimitReached && authCfg.screenEnabled) {
      uartProtocol.sendAlarmRaised("ORP_LIMIT", "Limite journalière ORP/chlore atteinte");
    }
  } else if (lastOrpLimitReached && authCfg.screenEnabled) {
    uartProtocol.sendAlarmCleared("ORP_LIMIT");
  }
  lastOrpLimitReached = safetyLimits.orpLimitReached;

  // Vérifier valeurs capteurs aberrantes — log et alerte MQTT à la transition seulement
  float ph = sensors.getPh();
  float orp = sensors.getOrp();
  float temp = sensors.getTemperature();

  static bool lastPhAbnormal   = false;
  static bool lastOrpAbnormal  = false;
  static bool lastTempAbnormal = false;

  bool phAbnormal   = (ph < 5.0f || ph > 9.0f);
  bool orpAbnormal  = (orp < 400.0f || orp > 900.0f);
  bool tempAbnormal = (!isnan(temp) && (temp < 5.0f || temp > 40.0f));

  if (phAbnormal && !lastPhAbnormal) {
    if (authCfg.sensorLogsEnabled) systemLogger.warning("Valeur pH anormale: " + String(ph));
    mqttManager.publishAlert("ph_abnormal", "pH=" + String(ph));
  } else if (!phAbnormal && lastPhAbnormal) {
    if (authCfg.sensorLogsEnabled) systemLogger.info("Valeur pH revenue à la normale: " + String(ph, 2));
  }
  lastPhAbnormal = phAbnormal;

  if (orpAbnormal && !lastOrpAbnormal) {
    if (authCfg.sensorLogsEnabled) systemLogger.warning("Valeur ORP anormale: " + String(orp));
    mqttManager.publishAlert("orp_abnormal", "ORP=" + String(orp));
  } else if (!orpAbnormal && lastOrpAbnormal) {
    if (authCfg.sensorLogsEnabled) systemLogger.info("Valeur ORP revenue à la normale: " + String(orp, 0) + " mV");
  }
  lastOrpAbnormal = orpAbnormal;

  if (tempAbnormal && !lastTempAbnormal) {
    if (authCfg.sensorLogsEnabled) systemLogger.warning("Température anormale: " + String(temp));
    mqttManager.publishAlert("temp_abnormal", "Temp=" + String(temp) + "°C");
  } else if (!tempAbnormal && lastTempAbnormal) {
    if (authCfg.sensorLogsEnabled) systemLogger.info("Température revenue à la normale: " + String(temp, 1) + " °C");
  }
  lastTempAbnormal = tempAbnormal;

  if (authCfg.sensorLogsEnabled) {
    systemLogger.debug("Health check OK - Heap: " + String(freeHeap) + " bytes");
  }
}

