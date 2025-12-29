#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPAsyncWiFiManager.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

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
void resetWiFiSettings();
void applyTimeConfig();
void checkSystemHealth();
void checkPasswordResetButton();

namespace {
const char kPortalIconSvg[] PROGMEM = R"rawliteral(
<svg xmlns="http://www.w3.org/2000/svg" width="192" height="192" viewBox="0 0 192 192" role="img" aria-label="PoolController">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#6aa8ff"/>
      <stop offset="1" stop-color="#2f6dff"/>
    </linearGradient>
  </defs>
  <rect width="192" height="192" rx="36" fill="url(#g)"/>
  <circle cx="96" cy="88" r="42" fill="#ffffff" opacity="0.9"/>
  <path d="M62 120c10 12 23 18 34 18s24-6 34-18" fill="none" stroke="#ffffff" stroke-width="10" stroke-linecap="round"/>
  <path d="M76 84a10 10 0 1 1 20 0" fill="none" stroke="#2f6dff" stroke-width="8" stroke-linecap="round"/>
  <path d="M96 84a10 10 0 1 1 20 0" fill="none" stroke="#2f6dff" stroke-width="8" stroke-linecap="round"/>
</svg>
)rawliteral";

void sendPortalIcon(AsyncWebServerRequest *request) {
  
  if (LittleFS.exists("/android-chrome-192x192.png")) {
    
    request->send(LittleFS, "/android-chrome-192x192.png", "image/png");
    
    return;
  }

  static bool warned = false;
  if (!warned) {
    systemLogger.warning("Icône portail AP absente dans LittleFS, fallback SVG utilisé");
    warned = true;
  }
  AsyncWebServerResponse *response = request->beginResponse_P(200, "image/svg+xml", kPortalIconSvg);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}
}  // namespace

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

  // Servir l'icône de l'application pour le portail AP
  httpServer.on("/android-chrome-192x192.png", HTTP_GET, sendPortalIcon);
  httpServer.on("/favicon.ico", HTTP_GET, sendPortalIcon);
  httpServer.on("/apple-touch-icon.png", HTTP_GET, sendPortalIcon);

  // Style custom for AP portal to align with app UI
  const char* portalStyle = R"rawliteral(
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<style>
  :root {
    --bg: #f5f7fa;
    --panel: #ffffff;
    --panel2: #f8f9fb;
    --stroke: rgba(0, 0, 0, 0.08);
    --text: #1a1d29;
    --muted: #6b7280;
    --accent: #4f8fff;
    --radius: 16px;
    --shadow: 0 2px 12px rgba(0, 0, 0, 0.08);
  }
  html, body { height: 100%; }
  body {
    margin: 0;
    padding: 28px 18px 40px;
    background: radial-gradient(1200px 600px at 20% -10%, #e9f1ff 0%, var(--bg) 45%, #eef2f7 100%);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Helvetica Neue", Arial, sans-serif;
    text-align: center;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: flex-start;
    min-height: 100vh;
    box-sizing: border-box;
    overflow-x: hidden;
  }
  * {
    box-sizing: border-box;
  }
  body > div {
    width: 100%;
    max-width: 420px;
  }
  h1 { display: none; }
  h3 { display: none; }
  a { color: var(--accent); text-decoration: none; }
  a:hover { text-decoration: underline; }
  form {
    width: 100%;
    margin: 14px auto;
    padding: 16px;
    background: var(--panel);
    border: 1px solid var(--stroke);
    border-radius: var(--radius);
    box-shadow: var(--shadow);
  }
  form.portalap {
    padding: 24px;
  }
  input, select {
    width: 100%;
    padding: 10px 12px;
    border: 1px solid var(--stroke);
    border-radius: 12px;
    background: var(--panel2);
    color: var(--text);
  }
  button {
    border: 0;
    border-radius: 12px;
    background-color: var(--accent);
    color: #fff;
    line-height: 2.6rem;
    font-size: 1rem;
    width: 100%;
    font-weight: 600;
  }
  form[action="/r"] button {
    background-color: #ff3b30;
  }
  .q {
    float: none;
    display: inline-block;
    min-width: 52px;
    text-align: right;
    color: var(--muted);
  }
  a[href="#p"] {
    display: inline-block;
    padding: 8px 10px;
    margin: 6px 0;
    background: var(--panel);
    border: 1px solid var(--stroke);
    border-radius: 10px;
  }
  a[href="#p"] + .q { margin-left: 8px; }
  .c { text-align: center; }
  body > div > div {
    width: 100%;
    margin: 0;
  }
  .pc-header {
    display: grid;
    gap: 6px;
    justify-items: center;
    margin: 4px 0 0;
    text-align: center;
    width: 100%;
  }
  .pc-logo {
    width: 72px;
    height: 72px;
    border-radius: 16px;
  }
  .pc-title {
    font-weight: 750;
    letter-spacing: 0.2px;
    font-size: 18px;
  }
  .pc-sub {
    font-size: 12px;
    color: var(--muted);
  }
  .pc-card {
    width: 100%;
    max-width: 420px;
    margin: -10px auto;
    padding: 16px;
    background: var(--panel);
    border: 1px solid var(--stroke);
    border-radius: var(--radius);
    box-shadow: var(--shadow);
  }
  .pc-center {
    text-align: center;
  }
  .pc-actions form {
    margin: 10px 0 0;
    padding: 0;
    border: 0;
    box-shadow: none;
    background: transparent;
  }
  form[action="/wifi"]:not([data-pc]),
  form[action="/0wifi"]:not([data-pc]),
  form[action="/i"]:not([data-pc]),
  form[action="/r"]:not([data-pc]) {
    display: none;
  }
</style>
<script>
  document.addEventListener('DOMContentLoaded', () => {
    document.body.classList.add('pc-portal');
    const h1 = document.querySelector('h1');
    const apName = h1 ? h1.textContent.trim() : '';
    const header = document.createElement('div');
    header.className = 'pc-header';
    header.innerHTML =
      '<div class="pc-title">PoolController</div>' +
      '<div class="pc-sub">Point d\'accès: ' + (apName || 'PoolControllerAP') + '</div>';
    document.body.insertBefore(header, document.body.firstChild);

    const ssid = document.querySelector('input#s');
    if (ssid) {
      ssid.placeholder = 'Nom du réseau (SSID)';
      const label = document.createElement('div');
      label.className = 'pc-sub';
      label.style.marginBottom = '6px';
      label.textContent = 'Réseau Wi-Fi';
      ssid.parentNode.insertBefore(label, ssid);
    }
    const pass = document.querySelector('input#p');
    if (pass) {
      pass.placeholder = 'Mot de passe';
      const label = document.createElement('div');
      label.className = 'pc-sub';
      label.style.marginBottom = '6px';
      label.textContent = 'Mot de passe';
      pass.parentNode.insertBefore(label, pass);
    }

    document.querySelectorAll('button').forEach((btn) => {
      if (btn.textContent.trim().toLowerCase() === 'save') {
        btn.textContent = 'Enregistrer';
      }
    });

    if (document.title === 'Options' || document.title === 'Config ESP' || document.title === 'Credentials Saved') {
      document.title = 'Configuration Wi-Fi';
    }

    const bodyText = document.body.textContent || '';
    if (bodyText.includes('No networks found. Refresh to scan again')) {
      const card = document.createElement('div');
      card.className = 'pc-card pc-center';
      card.innerHTML = '<div class="pc-title">Aucun réseau trouvé</div>' +
        '<div class="pc-sub">Actualisez la page pour relancer le scan.</div>';
      document.body.insertBefore(card, document.body.firstChild.nextSibling);
    }

    const staticFields = [
      { id: 'ip', label: 'Adresse IP fixe' },
      { id: 'gw', label: 'Passerelle' },
      { id: 'sn', label: 'Masque de sous-réseau' },
      { id: 'dns1', label: 'DNS primaire' },
      { id: 'dns2', label: 'DNS secondaire' }
    ];
    staticFields.forEach((f) => {
      const el = document.getElementById(f.id);
      if (el && !el.placeholder) {
        el.placeholder = f.label;
      }
    });

    const text = document.body.textContent || '';
    if (text.includes('Credentials Saved')) {
      while (document.body.children.length > 1) {
        document.body.removeChild(document.body.lastChild);
      }
      const card = document.createElement('div');
      card.className = 'pc-card pc-center';
      card.innerHTML = '<div class="pc-title">Identifiants enregistrés</div>' +
        '<div class="pc-sub">Connexion au réseau en cours. Si cela échoue, reconnectez-vous au point d&apos;accès.</div>';
      document.body.appendChild(card);
    }

    // Styliser la page Infos système
    const dlElements = document.querySelectorAll('dl');
    if (dlElements.length > 0 && document.title.includes('Info')) {
      document.title = 'Informations système';
      dlElements.forEach((dl) => {
        const wrapper = document.createElement('div');
        wrapper.className = 'pc-card';
        dl.parentNode.insertBefore(wrapper, dl);
        wrapper.appendChild(dl);

        // Styliser les éléments dt/dd
        const style = document.createElement('style');
        style.textContent = `
          .pc-card dl { margin: 0; }
          .pc-card dt {
            font-weight: 600;
            color: var(--text);
            margin-top: 12px;
            font-size: 13px;
          }
          .pc-card dt:first-child { margin-top: 0; }
          .pc-card dd {
            margin: 4px 0 0 0;
            color: var(--muted);
            font-size: 14px;
            word-break: break-all;
          }
        `;
        document.head.appendChild(style);
      });
    }
  });
</script>
  )rawliteral";
  wm.setCustomHeadElement(portalStyle);
  const char* portalOptions = R"rawliteral(
    <div class="pc-card pc-actions">
      <div class="pc-title">Configurer le Wi-Fi</div>
      <div class="pc-sub">Choisissez un réseau puis enregistrez les identifiants.</div>
      <form data-pc="1" action="/wifi" method="get"><button>Rechercher les réseaux</button></form>
      <form data-pc="1" action="/0wifi" method="get"><button>Configurer sans scan</button></form>
      <form data-pc="1" action="/i" method="get"><button>Infos système</button></form>
      <form data-pc="1" action="/r" method="post"><button>Réinitialiser la configuration</button></form>
    </div>
  )rawliteral";
  wm.setCustomOptionsElement(portalOptions);

  systemLogger.info("Tentative connexion WiFi...");

  // autoConnect peut bloquer assez longtemps pour déclencher le watchdog
  esp_task_wdt_delete(NULL);

  if (!wm.autoConnect("PoolControllerAP", "12345678")) {
    systemLogger.error("Échec connexion WiFi");
    currentWifiMode = WiFi.getMode();
    esp_task_wdt_add(NULL);
    return false;
  }

  systemLogger.info("WiFi connecté: " + WiFi.SSID());
  systemLogger.info("IP: " + WiFi.localIP().toString());
  currentWifiMode = WiFi.getMode();
  esp_task_wdt_add(NULL);
  return true;
}

void resetWiFiSettings() {
  systemLogger.warning("Effacement des credentials WiFi...");

  // Méthode 1: Effacer via WiFi.disconnect()
  WiFi.disconnect(true, true);  // disconnect(wifioff=true, eraseap=true)
  delay(100);

  // Méthode 2: Effacer l'espace de stockage Preferences utilisé par AsyncWiFiManager
  // AsyncWiFiManager utilise le namespace "wifi" dans les Preferences
  Preferences preferences;
  if (preferences.begin("wifi", false)) {  // false = read/write
    preferences.clear();  // Effacer TOUTES les données du namespace "wifi"
    preferences.end();
    systemLogger.info("Preferences 'wifi' effacées");
  } else {
    systemLogger.warning("Impossible d'ouvrir Preferences 'wifi'");
  }

  // Méthode 3: Effacer aussi les credentials WiFi natifs ESP32 (NVS partition nvs.net80211)
  // Cela garantit un nettoyage complet
  WiFi.mode(WIFI_OFF);
  delay(100);

  systemLogger.info("Credentials WiFi effacés - Mode AP sera activé au prochain démarrage");
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

void checkPasswordResetButton() {
  // Initialiser le bouton BOOT (GPIO0) et la LED
  pinMode(kBootButtonPin, INPUT_PULLUP);
  pinMode(kBuiltinLedPin, OUTPUT);
  digitalWrite(kBuiltinLedPin, LOW);  // LED éteinte par défaut

  // Vérifier si le bouton BOOT est maintenu enfoncé (actif bas)
  if (digitalRead(kBootButtonPin) == HIGH) {
    // Bouton relâché, pas de réinitialisation
    return;
  }

  systemLogger.warning("Bouton BOOT détecté enfoncé au démarrage");
  systemLogger.info("Maintenez enfoncé pendant 10s pour réinitialiser le mot de passe...");

  unsigned long startTime = millis();
  bool resetConfirmed = true;

  // Faire clignoter la LED pendant 10 secondes
  while (millis() - startTime < kPasswordResetButtonHoldMs) {
    // Vérifier que le bouton est toujours enfoncé
    if (digitalRead(kBootButtonPin) == HIGH) {
      resetConfirmed = false;
      systemLogger.info("Bouton relâché - Réinitialisation annulée");
      break;
    }

    // Clignoter la LED (100ms ON / 100ms OFF)
    digitalWrite(kBuiltinLedPin, (millis() / 100) % 2);
    delay(50);
  }

  // Éteindre la LED
  digitalWrite(kBuiltinLedPin, LOW);

  if (resetConfirmed) {
    systemLogger.critical("=== RÉINITIALISATION MOT DE PASSE CONFIRMÉE ===");

    // Charger la config actuelle
    loadMqttConfig();

    // Réinitialiser le mot de passe
    authCfg.adminPassword = "admin";

    // Sauvegarder la config avec le nouveau mot de passe
    saveMqttConfig();

    systemLogger.critical("Mot de passe réinitialisé à 'admin'");
    systemLogger.warning("Changement de mot de passe obligatoire au prochain login");

    // Faire clignoter rapidement la LED 5 fois pour confirmer
    for (int i = 0; i < 10; i++) {
      digitalWrite(kBuiltinLedPin, i % 2);
      delay(200);
    }
  }
}
