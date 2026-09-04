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
#include "ws_manager.h"
#include "history.h"
#include "measure_validity.h"
#include "version.h"
#include "rtc_manager.h"
#include "self_heal_logic.h"
#include "net_health_logic.h"
#include "heap_ring.h"
#include "heap_metrics.h"  // mesure 8 bits (mémoire réellement allouable) — trace de calibrage v2.19.0
#include "oom_guard.h"
#include "ping/ping_sock.h"  // esp_ping : sonde ICMP passerelle (watchdog connectivité v2.9.2)

// Variables globales
DNSServer dns;
AsyncWebServer httpServer(kHttpServerPort);
unsigned long lastMqttPublish = 0;
wifi_mode_t currentWifiMode = WIFI_MODE_NULL;
bool ntpSyncedOnce = false;  // Flag pour éviter sync RTC multiple

// mDNS : IP de la dernière annonce. Sert à détecter un changement d'IP (roaming
// CPL/mesh sans perte de WiFi) afin de ré-annoncer mDNS et de ne jamais publier une
// IP périmée sur poolcontroller.local.
// Race bénigne assumée : écrit par reannounceMdns() (loopTask) et lu/écrit par le
// handler d'événement WiFi (tâche event). Accès 32 bits aligné = atomique sur Xtensa
// (pas de tearing) ; le pire cas est une ré-annonce parasite/manquée, auto-corrigée
// au tick suivant. Le signal fiable passe par le drapeau `volatile` ci-dessous.
IPAddress mdnsLastAnnouncedIp;
// Drapeau posé par le handler d'événement WiFi (hors loopTask), consommé dans
// checkSystemHealth() : la ré-annonce mDNS réelle se fait dans le contexte loopTask.
volatile bool mdnsReannounceRequested = false;
// Debounce ré-annonce mDNS (v2.9.1) : dernier instant (millis) d'une ré-annonce
// EFFECTIVE, pour ignorer les oscillations d'IP du double NAT CPL. 0 = jamais.
unsigned long lastMdnsReannounceMs = 0;

// ===== Auto-guérison mémoire (v2.9.1) =====
// Compteur de reboots auto-guérison conservé en RAM RTC : survit à ESP.restart()
// (pas à un power-on / brownout). Un marqueur de validité distingue le premier boot
// (RAM RTC non initialisée) d'un reboot logiciel. La fenêtre (ancrée au 1er reboot,
// tumbling) est bornée sur l'epoch (millis() est remis à zéro à chaque restart).
constexpr uint32_t kAutoHealRtcMagic = 0xA5C0FFEEu;
RTC_NOINIT_ATTR uint32_t rtcAutoHealMagic;
RTC_NOINIT_ATTR uint8_t  rtcAutoHealRebootCount;
RTC_NOINIT_ATTR uint32_t rtcAutoHealWindowStartEpoch;  // epoch (s) du 1er reboot de la fenêtre
// Mode maintenance (RAM) : posé après kAutoHealMaxReboots reboots dans la fenêtre.
// DÉSACTIVE tout auto-reboot ultérieur et inhibe le dosage (via PumpController).
// L'appareil reste joignable (web/MQTT) pour diagnostic. Effacé au prochain boot.
bool maintenanceMode = false;
// Dernier échantillon de maxBloc (bug-badalloc-ws-broadcast) : écrit par
// checkHeapHealth(), lu par loop() pour calculer la cadence adaptative via
// heapSampleIntervalMs(). Sentinelle UINT32_MAX = pas encore d'échantillon →
// premier intervalle = kHealthCheckIntervalMs (cadence normale).
static uint32_t lastHeapMaxAllocSample = UINT32_MAX;

// ===== Watchdog de connectivité (v2.9.2) =====
// Dernier signe de vie réseau (millis). Signal PRIMAIRE : le callback ICMP
// on_ping_success de la sonde esp_ping vers la passerelle. Signaux SECONDAIRES (défense
// en profondeur, via notifyNetworkActivity()) : activité HTTP/WS effectivement servie,
// publication MQTT ACK par le broker, resync NTP réussie. But : ne rebooter QUE sur
// silence réseau TOTAL (vrai wedge lwIP). HYPOTHÈSE ASSUMÉE : si le réseau est
// totalement muet longtemps (aucun client web/WS, MQTT off, ICMP passerelle filtré),
// le watchdog reboote — comportement voulu, on ne peut pas distinguer ce cas d'un vrai
// gel de la pile. volatile : écrit par le thread esp_ping, la tâche serveur web async,
// la tâche MQTT et loopTask ; lu dans checkSystemHealth() (loopTask). Accès 32 bits
// aligné = atomique sur Xtensa (pas de tearing).
volatile unsigned long lastNetworkOkMs = 0;
// Session ICMP persistante (esp_ping) et passerelle sondée. UNE seule session infinie
// (count=0, cadence interne kNetworkProbeIntervalMs) → aucune fuite si la pile lwIP se
// fige. Recréée uniquement si la passerelle change (roaming subnet). nullptr = absente.
static esp_ping_handle_t netPingHandle = nullptr;
static IPAddress netProbeGateway;

// Déclaration des fonctions
bool setupWiFi();
void resetWiFiSettings();
void applyTimeConfig();
void checkSystemHealth();
void checkHeapHealth();
void onNtpTimeSync();
void notifyNetworkActivity();
void reannounceMdns(const char* reason);
void initAutoHealRtcState();
void performFailSafeRestart(const char* reason, const char* alertType, const String& alertMsg);
void performAutoHealRestart(uint32_t maxAllocHeap);
void enterMaintenanceMode();
void ensureNetworkProbe();
void handleHeapCorruptionDetected();

void setup() {
  // Parachute OOM (bug-oom-accept-http-durcissement, crash du 11/08) : réserve
  // d'urgence allouée AU PLUS TÔT — avant Serial/logger/FS — pour prendre un
  // bloc contigu avant toute fragmentation. N'émet aucun log (drainé en loop).
  initOomGuard();

  Serial.begin(115200);
  delay(kSerialInitDelayMs);

  systemLogger.begin();  // Initialise le mutex avant tout accès multi-core
  systemLogger.info("=== Démarrage ESP32 Pool Controller v" + String(FIRMWARE_VERSION) + " ===");
  // Armement à deux paliers (v2.18.8) : la taille RÉELLEMENT armée est
  // journalisée avec le maxBloc du moment (le palier de repli signale un tas
  // déjà contraint au boot).
  size_t oomArmedBytes = oomGuardReserveArmedBytes();
  if (oomArmedBytes > 0) {
    systemLogger.info("Parachute OOM armé : réserve d'urgence de " +
                      String(oomArmedBytes) + " o (" +
                      String(oomArmedBytes >= kOomReserveBytes ? "palier nominal"
                                                              : "palier de repli") +
                      // Métrique 8 bits : la même que les seuils du parachute (bug-080).
                      ") pré-allouée, maxBloc8=" + String(heapLargestFreeBlock8Bit()) + " o");
  } else {
    // Improbable au boot : les deux mallocs refusés — la loop réarmera (hystérésis).
    systemLogger.warning("Parachute OOM : réserve non allouée au boot (nominal ET repli refusés) "
                         "— armement différé en loop");
  }
  systemLogger.info("Build: " + String(FIRMWARE_BUILD_DATE) + " " + String(FIRMWARE_BUILD_TIME));
  // feature-066 : détection PSRAM (module WROVER-E). psramFound() est fiable
  // ici : l'init a lieu avant setup() (BOARD_HAS_PSRAM) et échoue proprement
  // sur un module WROOM sans PSRAM (binaire unique, dégradation gracieuse).
  if (psramFound()) {
    systemLogger.info("PSRAM : " + String(ESP.getPsramSize() / 1048576) +
                      " Mo détectés (les allocations > 4 Ko y sont placées automatiquement)");
  } else {
    systemLogger.info("PSRAM absente — RAM interne seule (module WROOM)");
  }

  // Initialisation GPIO bouton factory reset et LED intégrée
  // PCB v2 (feature-019) : bouton factory reset sur GPIO35 (input-only,
  // sans pull-up interne possible) avec pull-up externe 10 kΩ vers 3V3 → actif bas.
  pinMode(kFactoryResetButtonPin, INPUT);
  pinMode(kBuiltinLedPin, OUTPUT);
  digitalWrite(kBuiltinLedPin, LOW);

  // Initialisation watchdog
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Migration core 3 (feature-063) : IDF 5 remplace (timeout, panic) par une
  // struct de config. Le TWDT est déjà initialisé par le core Arduino 3 →
  // esp_task_wdt_reconfigure (esp_task_wdt_init renverrait ESP_ERR_INVALID_STATE).
  {
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = kWatchdogTimeoutSec * 1000,
        .idle_core_mask = 0,   // idem core 2 : pas d'abonnement des tâches idle
        .trigger_panic = true, // idem core 2 : panic → reset au timeout
    };
    esp_task_wdt_reconfigure(&wdtConfig);
  }
#else
  esp_task_wdt_init(kWatchdogTimeoutSec, true);
#endif
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
  // feature-053 : restaure l'état du Mode Boost (survit à un reboot dans la
  // journée ; expiré au chargement si minuit dépassé et heure valide).
  loadBoostState();

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

  // Auto-guérison (v2.9.1) : initialise le compteur de reboots en RAM RTC. Placé
  // APRÈS l'application de l'heure RTC pour que la fenêtre epoch soit fiable.
  initAutoHealRtcState();

  // feature-091 (C3 pool-chemistry) : décision de dispense de la première purge
  // après une coupure COURTE (mémoire RTC + esp_reset_reason). APRÈS
  // rtcManager.applyToSystem() (l'heure système doit être valide pour mesurer la
  // coupure), AVANT le premier measureValidity.update() de la loop.
  measureValidity.begin();

  PumpController.begin();
  // feature-091 : l'arming legacy du timer de stabilisation au boot (mode Powered)
  // est RETIRÉ — la stabilisation après reprise de circulation est portée par la
  // purge adaptative de measureValidity (plancher + stabilité de la mesure).
  filtration.begin();
  lighting.begin();
  history.begin();

  // Ring heap RTC noinit (bug-heap-toctou A4) : dump de la cinétique heap du boot
  // précédent si le reset n'est pas un POWERON. Placé APRÈS systemLogger.begin()
  // et le montage des FS (history.begin() a activé la persistance des logs → un
  // flush éventuel fonctionne), AVANT le WiFi (trace disponible au plus tôt).
  heapRingDumpIfCrash();

  // Post-mortem d'allocation RTC noinit (bug-parachute-oom-jamais-rearme) :
  // état du tas AU MOMENT EXACT du dernier échec d'allocation du boot précédent
  // + verdict (famine / suspicion de corruption). Placé juste APRÈS le dump de
  // l'anneau : les deux traces se lisent ensemble (l'anneau donne la cinétique,
  // le post-mortem le point d'échec). Lit l'instantané pris par initOomGuard(),
  // qui a déjà invalidé la zone RTC pour le boot courant.
  oomPostMortemDumpIfBoot();

  // Configuration WiFi
  bool wifiConnected = setupWiFi();

  if (wifiConnected) {
    // mDNS
    if (!MDNS.begin(kMdnsHostname)) {
      systemLogger.error("Échec démarrage mDNS");
    } else {
      MDNS.addService("http", "tcp", kMdnsHttpPort);
      mdnsLastAnnouncedIp = WiFi.localIP();  // Mémorise l'IP annoncée (détection roaming)
      systemLogger.info(String("mDNS: ") + kMdnsFullHost + " disponible");
    }

    // Initialisation MQTT
    mqttManager.begin();

    // feature-059 : alerte différée du disjoncteur de boot-loop history. La
    // décision et l'effacement ont eu lieu dans history.begin() (avant WiFi/MQTT,
    // outQueue inexistante à ce stade) → on publie ici, après mqttManager.begin().
    if (history.partitionWasAutoErased()) {
      mqttManager.publishAlert("history_partition_reset",
        "Partition historique corrompue effacée automatiquement (boot-loop détecté) — l'historique repart de zéro");
    }

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
  measureValidity.update();          // feature-061 : validité pH/ORP — AVANT history (le point 5 min lit le cache)
  history.update();
  systemLogger.update();
  updateManualInject();

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

  // Contrôle d'intégrité du tas (bug-parachute-oom-jamais-rearme) — PLACÉ APRÈS
  // PumpController.update() (condition pool-chemistry C2 : jamais entre
  // filtration.update() et PumpController.update(), qui doivent voir le même
  // tour). Gardes CUMULATIVES fail-closed avant tout parcours du tas : pas en
  // mode maintenance, pas pendant une OTA, aucun dosage auto, aucune injection
  // manuelle, AUCUNE pompe énergisée (isAnyPumpEnergized couvre le test manuel
  // armé depuis la tâche web, invisible de isPh/isOrpDosing), et uptime minimal.
  if (!maintenanceMode && !PumpController.isOtaInProgress() &&
      !PumpController.isPhDosing() && !PumpController.isOrpDosing() &&
      !manualInjectPh.active && !manualInjectOrp.active &&
      !PumpController.isAnyPumpEnergized() &&
      now >= kAutoHealMinUptimeMs) {
    if (oomGuardIntegrityTick()) {
      handleHeapCorruptionDetected();  // peut ne pas retourner (redémarrage propre)
    }
  }

  // Détection appui long bouton factory reset (10s) en cours de fonctionnement.
  // PCB v2 (feature-019) : bouton actif bas, pull-up externe 10 kΩ vers 3V3.
  // GPIO35 est input-only sans pull-up interne possible.
  // Comportement utilisateur identique à PCB v1 : appui maintenu = factory reset.
  {
    static unsigned long buttonPressStart = 0;
    static bool lastPressed = false;
    static bool ledState = false;
    static unsigned long lastLedToggle = 0;
    bool pressed = (digitalRead(kFactoryResetButtonPin) == HIGH);

    if (pressed && !lastPressed) {
      // Début d'appui
      buttonPressStart = now;
      systemLogger.info("Bouton reset enfoncé - maintenir 10s pour factory reset");
    } else if (pressed && buttonPressStart > 0) {
      unsigned long held = now - buttonPressStart;

      // Faire clignoter la LED pendant l'appui
      if (now - lastLedToggle >= 200) {
        ledState = !ledState;
        digitalWrite(kBuiltinLedPin, ledState);
        lastLedToggle = now;
        esp_task_wdt_reset();
      }

      // Factory reset après 10s
      if (held >= kFactoryResetButtonHoldMs) {
        systemLogger.critical("=== FACTORY RESET CONFIRMÉ (appui 10s) ===");
        // Clignotement rapide de confirmation (5 fois)
        for (int i = 0; i < 10; i++) {
          digitalWrite(kBuiltinLedPin, i % 2);
          delay(150);
        }
        digitalWrite(kBuiltinLedPin, LOW);
        resetWiFiSettings();
        delay(500);
        mqttManager.shutdownForRestart();  // ADR-0011 : flush status=offline + stop mqttTask
        ESP.restart();
      }
    } else if (!pressed && lastPressed) {
      // Relâché avant 10s
      if (buttonPressStart > 0 && (now - buttonPressStart) < kFactoryResetButtonHoldMs) {
        systemLogger.info("Bouton relâché - factory reset annulé");
      }
      buttonPressStart = 0;
      digitalWrite(kBuiltinLedPin, LOW);
    }

    lastPressed = pressed;
  }

  // Vérification santé système périodique
  static unsigned long lastHealthCheck = 0;
  if (now - lastHealthCheck >= kHealthCheckIntervalMs) {
    checkSystemHealth();
    lastHealthCheck = now;
    esp_task_wdt_reset();
  }

  // Auto-guérison heap sur timer PROPRE à cadence adaptative (bug-badalloc-ws-broadcast) :
  // 60 s en régime sain, kHeapCheckFastIntervalMs (5 s) dès que maxBloc passe sous
  // kLowHeapWarnBytes → le débounce (3 ticks) confirme en ~15 s au lieu de 180 s.
  // Premier passage : lastHeapMaxAllocSample = UINT32_MAX (sentinelle) → cadence normale.
  static unsigned long lastHeapCheckMs = 0;
  if (now - lastHeapCheckMs >= heapSampleIntervalMs(lastHeapMaxAllocSample,
                                                    kLowHeapWarnBytes,
                                                    kHealthCheckIntervalMs,
                                                    kHeapCheckFastIntervalMs)) {
    checkHeapHealth();
    lastHeapCheckMs = now;
  }

  // Ring heap RTC noinit (bug-heap-toctou A4) : échantillon toutes les
  // kHeapRingSampleIntervalMs (2 s) — cinétique fine d'effondrement du heap,
  // dumpée au boot suivant si reset anormal (heapRingDumpIfCrash).
  static unsigned long lastHeapRingSampleMs = 0;
  if (now - lastHeapRingSampleMs >= kHeapRingSampleIntervalMs) {
    heapRingSample();
    lastHeapRingSampleMs = now;
  }

  // Parachute OOM (bug-oom-accept-http-durcissement) : émission différée des
  // logs posés par le new_handler/callback heap_caps + réarmement de la réserve
  // à deux paliers (nominal ≥ kOomRearmMinMaxAllocBytes, repli
  // ≥ kOomFallbackRearmMinMaxAllocBytes ; cadence interne 10 s).
  oomGuardLoopTick();

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
        // Invalide l'IP mDNS mémorisée : le service mDNS ne survit pas à une
        // déconnexion WiFi sur ESP32. Le prochain GOT_IP déclenchera donc une
        // ré-annonce, même si l'AP réattribue exactement la même IP.
        mdnsLastAnnouncedIp = IPAddress();
        break;
      }
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        systemLogger.info("WiFi associé à l'AP");
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
        IPAddress newIp = WiFi.localIP();
        systemLogger.info("WiFi IP obtenue: " + newIp.toString() +
                          " RSSI=" + String(WiFi.RSSI()) + "dBm");
        // Roaming CPL/mesh : l'ESP32 peut changer d'IP sans que WiFi.isConnected()
        // ne passe à false. Si l'IP a réellement changé (ou a été invalidée par une
        // déconnexion), on demande une ré-annonce mDNS. On ne fait QUE poser un
        // drapeau : ce handler tourne sur la tâche événements WiFi et MDNS/lwIP n'est
        // pas thread-safe vis-à-vis du serveur web async. Le travail réel a lieu dans
        // checkSystemHealth() (contexte loopTask).
        if (newIp != mdnsLastAnnouncedIp) {
          mdnsReannounceRequested = true;
        }
        break;
      }
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

// Point d'entrée public (v2.9.2) : signale une activité réseau réussie depuis
// n'importe quel module (MQTT ACK, requête HTTP/WS servie, resync NTP). Rafraîchit
// l'horloge de stall du watchdog de connectivité pour ne rebooter que sur silence
// réseau TOTAL. Écriture 32 bits atomique (voir lastNetworkOkMs) : sûr multi-tâches.
void notifyNetworkActivity() {
  lastNetworkOkMs = millis();
}

// Callback appelé quand NTP se synchronise
void onNtpTimeSync() {
  // Signal réseau secondaire : une resync NTP prouve que la pile UDP/DNS répond.
  notifyNetworkActivity();
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

// Ré-annonce le service mDNS (MDNS.end + begin + addService HTTP), en réutilisant
// exactement la séquence d'enregistrement de l'init (setup). À appeler UNIQUEMENT
// depuis la loop (contexte loopTask) : MDNS/lwIP n'est pas thread-safe vis-à-vis du
// serveur web async ni du handler d'événements WiFi.
void reannounceMdns(const char* reason) {
  MDNS.end();
  if (MDNS.begin(kMdnsHostname)) {
    MDNS.addService("http", "tcp", kMdnsHttpPort);
    mdnsLastAnnouncedIp = WiFi.localIP();
    systemLogger.info("mDNS ré-annoncé sur " + mdnsLastAnnouncedIp.toString() +
                      " (" + String(reason) + ")");
  } else {
    systemLogger.warning("Échec ré-annonce mDNS");
  }
}

// Initialise l'état d'auto-guérison en RAM RTC (v2.9.1). Au 1er boot (RAM RTC non
// initialisée : marqueur absent) le compteur repart à zéro. Sur un reboot logiciel,
// on purge le compteur si la fenêtre a expiré (dernier reboot > fenêtre), ce qui
// autorise à nouveau l'auto-guérison après une période saine.
//
// SÉMANTIQUE DE LA FENÊTRE (tumbling, PAS glissante) : elle est ANCRÉE à l'epoch du
// 1er reboot de la série (rtcAutoHealWindowStartEpoch, posé une seule fois dans
// performAutoHealRestart quand le compteur vaut 0) et compte kAutoHealRebootWindowMs
// à partir de ce point fixe. Elle n'est PAS ré-armée à chaque reboot.
//
// FAIL-SAFE VOULU : la purge exige un epoch valide (nowEpoch >= kMinValidEpoch). Si
// l'heure n'est JAMAIS valide (RTC non réglée ET pas de NTP), la fenêtre n'est jamais
// purgée → le compteur ne redescend pas → on bascule PLUS TÔT en mode maintenance
// (auto-guérison désactivée, dosage suspendu). C'est le comportement conservateur
// souhaité : sans référence temporelle fiable, mieux vaut arrêter d'enchaîner les
// reboots que risquer une boucle.
void initAutoHealRtcState() {
  if (rtcAutoHealMagic != kAutoHealRtcMagic) {
    // Premier boot après power-on / brownout : RAM RTC non fiable → tout à zéro.
    rtcAutoHealMagic = kAutoHealRtcMagic;
    rtcAutoHealRebootCount = 0;
    rtcAutoHealWindowStartEpoch = 0;
    systemLogger.info("Auto-guérison : état initialisé (premier boot)");
    return;
  }

  // Reboot logiciel : compteur conservé, purgé si la fenêtre (ancrée au 1er reboot)
  // a expiré. Purge conditionnée à un epoch valide — cf. fail-safe ci-dessus.
  time_t nowEpoch = time(nullptr);
  if (rtcAutoHealRebootCount > 0 && nowEpoch >= kMinValidEpoch &&
      rtcAutoHealWindowStartEpoch != 0) {
    uint32_t elapsedS = (uint32_t)nowEpoch - rtcAutoHealWindowStartEpoch;
    if (elapsedS > (kAutoHealRebootWindowMs / 1000UL)) {
      systemLogger.info("Auto-guérison : fenêtre expirée (" + String(elapsedS) +
                        "s), compteur reboots réinitialisé");
      rtcAutoHealRebootCount = 0;
      rtcAutoHealWindowStartEpoch = 0;
    }
  }
  if (rtcAutoHealRebootCount > 0) {
    systemLogger.warning("Auto-guérison : " + String(rtcAutoHealRebootCount) +
                         " reboot(s) auto récent(s) dans la fenêtre courante");
  }
}

// Redémarrage propre fail-safe (v2.9.2). SÉQUENCE EXACTE imposée par pool-chemistry
// (condition C1) — NE PAS réordonner ni modifier le contenu. Ne retourne jamais.
// Générique : utilisée par l'auto-guérison heap ET par le watchdog de connectivité,
// avec un motif/type d'alerte propre à chaque appelant.
void performFailSafeRestart(const char* reason, const char* alertType,
                            const String& alertMsg) {
  // 1. Coupe les 2 pompes immédiatement (sécurité chimique avant tout).
  PumpController.stopAll();
  // 2. Persiste les compteurs journaliers de dosage (perte ~0 mL au reboot).
  saveDailyCounters();
  // 3. Incrémente le compteur RTC et ancre la fenêtre (tumbling) au 1er reboot.
  if (rtcAutoHealRebootCount == 0) {
    time_t nowEpoch = time(nullptr);
    rtcAutoHealWindowStartEpoch = (nowEpoch >= kMinValidEpoch) ? (uint32_t)nowEpoch : 0;
  }
  rtcAutoHealRebootCount++;
  // 4. Log critique (persisté).
  systemLogger.critical(reason);
  // 5. Alerte MQTT si joignable — tolère l'échec, ne bloque pas.
  mqttManager.publishAlert(alertType, alertMsg);
  // 6. Flush disque — tolère un échec silencieux si le heap est trop bas.
  systemLogger.flushToDisk();
  // 7. Redémarrage.
  ESP.restart();
}

// Redémarrage propre sur heap critique (v2.9.1). Conserve le message/alerte d'origine
// et délègue la séquence C1 à performFailSafeRestart(). Ne retourne jamais.
void performAutoHealRestart(uint32_t maxAllocHeap) {
  String reason = "Auto-guérison : heap critique (fragmentation), "
                  "redémarrage propre — maxBloc8=" + String(maxAllocHeap) + " bytes";
  performFailSafeRestart(reason.c_str(), "auto_reboot",
      "Redémarrage auto-guérison (heap critique), maxBloc8=" + String(maxAllocHeap));
}

// Callback ICMP (v2.9.2) : invoqué par le thread interne esp_ping à chaque écho reçu
// de la passerelle → signe de vie réseau PRIMAIRE, indépendant de MQTT. Exécuté hors
// loopTask : ne fait QUE poser l'horodatage (accès 32 bits atomique).
static void onNetPingSuccess(esp_ping_handle_t /*hdl*/, void* /*args*/) {
  lastNetworkOkMs = millis();
}

// (Re)démarre la sonde ICMP persistante vers la passerelle (v2.9.2). Idempotent :
// ne fait rien si la session existe déjà pour la passerelle courante. Recrée proprement
// (stop + delete) si la passerelle a changé (roaming subnet) pour éviter toute fuite.
// À appeler UNIQUEMENT depuis checkSystemHealth() (contexte loopTask), WiFi associé.
void ensureNetworkProbe() {
  if (!WiFi.isConnected()) return;
  IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress((uint32_t)0)) return;  // passerelle pas encore connue (DHCP)
  if (netPingHandle != nullptr && gw == netProbeGateway) return;  // déjà en place

  // Passerelle changée ou session absente : on repart d'une session propre.
  if (netPingHandle != nullptr) {
    esp_ping_stop(netPingHandle);
    esp_ping_delete_session(netPingHandle);
    netPingHandle = nullptr;
  }

  ip_addr_t target;
  IP_ADDR4(&target, gw[0], gw[1], gw[2], gw[3]);

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = target;
  cfg.count = ESP_PING_COUNT_INFINITE;       // session persistante (aucune recréation)
  cfg.interval_ms = kNetworkProbeIntervalMs; // cadence interne de la sonde
  cfg.timeout_ms = kNetworkProbeTimeoutMs;

  esp_ping_callbacks_t cbs = {};
  cbs.cb_args = nullptr;
  cbs.on_ping_success = onNetPingSuccess;
  cbs.on_ping_timeout = nullptr;  // le stall est mesuré par l'absence de succès
  cbs.on_ping_end = nullptr;

  // Warning one-shot (v2.9.2) : ré-armé à chaque succès, émis une seule fois par
  // épisode d'échec pour ne pas noyer le journal à chaque tick (cf. flush heap one-shot).
  static bool probeStartWarnArmed = true;
  if (esp_ping_new_session(&cfg, &cbs, &netPingHandle) == ESP_OK &&
      esp_ping_start(netPingHandle) == ESP_OK) {
    netProbeGateway = gw;
    // Réarme l'horloge de stall au démarrage de la sonde pour ne pas compter le temps
    // écoulé avant son existence.
    lastNetworkOkMs = millis();
    probeStartWarnArmed = true;
    systemLogger.info("Sonde ICMP passerelle démarrée vers " + gw.toString());
  } else {
    if (netPingHandle != nullptr) {
      esp_ping_delete_session(netPingHandle);
      netPingHandle = nullptr;
    }
    if (probeStartWarnArmed) {
      systemLogger.warning("Échec démarrage sonde ICMP passerelle");
      probeStartWarnArmed = false;
    }
  }
}

// Bascule en mode maintenance (v2.9.1). Condition pool-chemistry (C2) : après trop
// de reboots auto dans la fenêtre, on N'ENCHAÎNE PLUS de reboot. On coupe et inhibe
// le dosage, on alerte, et on RESTE joignable pour diagnostic (pas de restart).
void enterMaintenanceMode() {
  if (maintenanceMode) return;  // idempotent
  maintenanceMode = true;
  PumpController.stopAll();
  PumpController.setMaintenanceInhibit(true);  // inhibe tout dosage ultérieur
  systemLogger.critical("Mode maintenance : " + String(rtcAutoHealRebootCount) +
      " reboots auto en " + String(kAutoHealRebootWindowMs / 60000UL) +
      " min — auto-guérison désactivée, dosage suspendu");
  mqttManager.publishAlert("maintenance_mode",
      "Auto-guérison désactivée après " + String(rtcAutoHealRebootCount) +
      " reboots — dosage suspendu");
}

// Chemin « corruption du tas détectée » (bug-parachute-oom-jamais-rearme,
// condition pool-chemistry C4). Appelé UNE SEULE FOIS : oomGuardIntegrityTick()
// désactive définitivement le contrôle après une détection.
// SÉQUENCE IMPOSÉE — ne pas réordonner :
//   1. sécurité chimique AVANT TOUT (coupe des 2 pompes + inhibition du dosage) ;
//   2. CRITICAL one-shot + flush disque (la trace doit survivre à un abort) ;
//   3. alerte MQTT heap_corruption (tolère l'échec) ;
//   4. sortie par le chemin PROPRE EXISTANT : performFailSafeRestart() tant que
//      le plafond de reboots de la fenêtre n'est pas atteint, sinon mode
//      maintenance (appareil joignable, dosage suspendu).
// JAMAIS d'ESP.restart() nu : un tas corrompu impose de couper le dosage avant
// toute autre action, et de persister les compteurs journaliers (fait par
// performFailSafeRestart).
void handleHeapCorruptionDetected() {
  // Sécurité chimique d'abord (condition pool-chemistry C4) : dosage coupé et
  // inhibé AVANT toute autre action, y compris dans la branche « maintenance ».
  PumpController.stopAll();
  PumpController.setMaintenanceInhibit(true);

  if (rtcAutoHealRebootCount < kAutoHealMaxReboots) {
    // performFailSafeRestart réalise déjà la séquence complète (stopAll,
    // persistance des compteurs, CRITICAL, alerte MQTT, flush, restart) : on ne
    // la double PAS ici (revue v2.18.8 : un seul arrêt, une seule alerte).
    performFailSafeRestart(
        "Corruption du tas DÉTECTÉE (heap_caps_check_integrity_all) — "
        "dosage coupé et inhibé, redémarrage propre",
        "heap_corruption",
        "Corruption du tas détectée — dosage suspendu, redémarrage propre");  // ne retourne pas
  }

  // Plafond de reboots atteint : on n'enchaîne pas, on reste en maintenance —
  // c'est ici (et seulement ici) qu'il faut journaliser et alerter nous-mêmes.
  systemLogger.critical("Corruption du tas DÉTECTÉE (heap_caps_check_integrity_all) "
                        "— dosage coupé et inhibé, plafond de redémarrages atteint : "
                        "passage en mode maintenance");
  systemLogger.flushToDisk();
  mqttManager.publishAlert("heap_corruption",
      "Corruption du tas détectée — dosage suspendu, mode maintenance (plafond de redémarrages)");
  enterMaintenanceMode();
}

// Surveillance heap + auto-guérison (bug-badalloc-ws-broadcast) : bloc extrait de
// checkSystemHealth() pour tourner sur un timer PROPRE à cadence adaptative (cf.
// heapSampleIntervalMs dans loop()). SOURCE UNIQUE de la mesure du plus gros bloc
// (heapLargestFreeBlock8Bit depuis bug-080/v2.33.0 — l'ancienne ESP.getMaxAllocHeap
// comptait l'IRAM 32 bits et rendait les gardes AVEUGLES) et de consecutiveBelow
// — checkSystemHealth() ne les touche plus (l'alerte kMinFreeHeapBytes
// porte sur freeHeap, métrique différente, et reste à 60 s). AUCUNE publication MQTT
// périodique ici (seules les alertes des chemins restart/maintenance).
// Invariants ADR-0029 conservés À L'IDENTIQUE : débounce kAutoHealDebounceChecks,
// budget kAutoHealMaxReboots, kAutoHealMinUptimeMs, mode maintenance, garde OTA.
void checkHeapHealth() {
  // ===== Instrumentation heap (v2.9.1) =====
  // maxBloc est ÉCHANTILLONNÉ à CHAQUE tick (requis pour la décision auto-heal et le
  // debounce), mais la trace INFO persistée (SANS le gate sensorLogsEnabled) n'est
  // ÉMISE que toutes les kHeapLogIntervalMs OU lors d'une chute significative
  // (> kHeapLogDropBytes depuis la dernière émission) → capture la tendance/descente
  // sans noyer le journal (~1440 lignes/j sinon). Voir la fragmentation : maxBloc
  // s'effondre pendant que libre reste correct.
  // bug-080 (v2.33.0) : point de mesure UNIQUE basculé sur la métrique 8 bits
  // RÉELLE. Ce même échantillon alimente le WARN+flush, le débounce auto-heal,
  // SelfHealInputs::maxAllocHeap et la cadence adaptative (lastHeapMaxAllocSample).
  uint32_t maxAllocHeap = heapLargestFreeBlock8Bit();
  // Export de l'échantillon pour la cadence adaptative du timer dans loop()
  // (bug-badalloc-ws-broadcast) — seule ligne ajoutée au bloc déplacé.
  lastHeapMaxAllocSample = maxAllocHeap;
  // Trace INFO périodique désactivée (2026-07-23, demande utilisateur) : la tendance
  // heap est suffisamment couverte par les alertes ci-dessous et l'auto-guérison.
  // Décommenter ce bloc pour ré-instrumenter (émission toutes les kHeapLogIntervalMs
  // ou sur chute > kHeapLogDropBytes). L'échantillonnage de maxAllocHeap ci-dessus
  // reste ACTIF : il alimente la décision auto-heal et le flush one-shot.
  // unsigned long heapNow = millis();
  // static unsigned long lastHeapLogMs = 0;
  // static uint32_t lastHeapLogMaxBloc = UINT32_MAX;
  // bool heapDroppedSignificantly =
  //     (maxAllocHeap + kHeapLogDropBytes < lastHeapLogMaxBloc);  // baisse > seuil (pas d'underflow)
  // if (lastHeapLogMs == 0 || (heapNow - lastHeapLogMs >= kHeapLogIntervalMs) ||
  //     heapDroppedSignificantly) {
  //   systemLogger.info("Heap: libre=" + String(ESP.getFreeHeap()) +
  //                     " maxBloc=" + String(maxAllocHeap) +
  //                     " minLibre=" + String(ESP.getMinFreeHeap()));
  //   lastHeapLogMs = heapNow;
  //   lastHeapLogMaxBloc = maxAllocHeap;
  // }

  // ===== Trace de calibrage des seuils mémoire (v2.19.0) =====
  // bug-metrique-heap-inclut-iram-32bits : ESP.getMaxAllocHeap() compte AUSSI
  // l'IRAM 32 bits inutilisable par un malloc/new (cf. heap_metrics.h). Cette
  // trace a servi à relever les vraies valeurs 8 bits ON-TARGET ; depuis bug-080
  // (v2.33.0), les gardes ci-dessous lisent la métrique 8 bits et leurs seuils
  // sont recalibrés sur ces relevés (nominal 21 492 o) — la trace reste utile
  // pour surveiller la dérive du régime. Valeurs BRUTES en octets, aucun arrondi.
  //
  // Le suivi des CREUX est alimenté par heapRingSample() (cadence 2 s, valeurs
  // déjà mesurées) et NON ici : à la cadence de checkHeapHealth() (60 s en
  // régime sain), on raterait des creux qui ne durent que quelques secondes —
  // le minimum serait trop optimiste et le seuil d'auto-guérison calibré dessus
  // déclencherait des redémarrages en régime normal (revue).
  // Deux minima sont tracés : depuis le BOOT (pire cas absolu, sature après le
  // premier creux profond) et sur la FENÊTRE écoulée depuis la trace précédente
  // (montre si les creux s'aggravent au fil des heures).
  //
  // v2.20.2 (bloc C) : cadence 5 min → kHeap8LogIntervalMs (30 min) et ligne
  // RACCOURCIE — la partie « interne: » a été RETIRÉE. Elle était figée (27 636 o
  // de reliquat d'IRAM 32 bits, identique à chaque relevé pendant 5 h) : elle a
  // servi une fois à PROUVER l'écart entre les deux métriques (ADR-0038), la
  // preuve est faite et consignée. La conserver coûtait ~70 o par ligne sur un
  // journal persistant de 8 Ko, dont la rétention était tombée à ~30 min — c'est
  // ce qui a détruit le post-mortem du crash du 15/08.
  unsigned long heap8Now = millis();
  static unsigned long lastHeap8LogMs = 0;
  if (lastHeap8LogMs == 0 || (heap8Now - lastHeap8LogMs >= kHeap8LogIntervalMs)) {
    lastHeap8LogMs = heap8Now;
    HeapMetrics m = heapMetricsSnapshot();
    uint32_t minBoot = heapMinMaxAlloc8SinceBoot();
    uint32_t minWindow = heapTakeMinMaxAlloc8Window();
    // snprintf sur tampon de PILE, pas de concaténation de String (revue) :
    // cette trace décrit une pénurie mémoire et peut donc s'émettre PENDANT
    // une pénurie — construire des String ici allouerait précisément quand
    // l'allocation échoue (c'est ainsi qu'un message d'erreur a tué le
    // contrôleur le 15/08 : log → console → mutex → abort).
    // « ws= » donne le contexte de charge (clients temps réel connectés), qui
    // distingue régime chargé et régime calme au moment du calibrage.
    char ligne[128];
    snprintf(ligne, sizeof(ligne),
             "Heap 8bits: libre8=%u maxBloc8=%u minBoot=%u minFen=%u ws=%u",
             (unsigned)m.free8, (unsigned)m.maxAlloc8, (unsigned)minBoot,
             (unsigned)minWindow, (unsigned)wsManager.clientCount());
    systemLogger.info(ligne);
  }

  // Flush disque one-shot au 1er franchissement à la baisse du seuil d'alerte : capture
  // la descente même si le restart n'est pas atteint. Ré-armé quand ça repasse au-dessus.
  static bool lowHeapFlushArmed = true;
  if (maxAllocHeap < kLowHeapWarnBytes) {
    if (lowHeapFlushArmed) {
      systemLogger.warning("Heap fragmenté sous seuil d'alerte: maxBloc8=" + String(maxAllocHeap) + " bytes");
      systemLogger.flushToDisk();
      lowHeapFlushArmed = false;
    }
  } else {
    lowHeapFlushArmed = true;
  }

  // ===== Auto-guérison mémoire (v2.9.1) =====
  // Maintient le compteur de ticks consécutifs sous le seuil de restart, puis délègue
  // la décision au module pur evaluateSelfHeal(). Aucun effet en mode maintenance.
  static uint8_t consecutiveBelow = 0;
  if (maxAllocHeap < kRestartMaxAllocHeapBytes) {
    if (consecutiveBelow < 255) consecutiveBelow++;
  } else {
    consecutiveBelow = 0;
  }

  // Garde OTA (v2.9.2) : ne rien évaluer pendant un flash — un pic de fragmentation ou
  // un stall transitoire ne doit JAMAIS déclencher un reboot au milieu d'une OTA.
  if (!maintenanceMode && !PumpController.isOtaInProgress()) {
    SelfHealInputs shIn;
    shIn.maxAllocHeap = maxAllocHeap;
    shIn.restartThreshold = kRestartMaxAllocHeapBytes;
    shIn.consecutiveBelow = consecutiveBelow;
    shIn.debounceChecks = kAutoHealDebounceChecks;
    shIn.uptimeMs = (uint32_t)millis();
    shIn.minUptimeMs = kAutoHealMinUptimeMs;
    shIn.rebootCountInWindow = rtcAutoHealRebootCount;
    shIn.maxReboots = kAutoHealMaxReboots;

    SelfHealDecision decision = evaluateSelfHeal(shIn);
    if (decision == SelfHealDecision::Restart) {
      performAutoHealRestart(maxAllocHeap);  // ne retourne pas (ESP.restart)
    } else if (decision == SelfHealDecision::EnterMaintenance) {
      enterMaintenanceMode();
    }
  }
}

void checkSystemHealth() {
  // Vérifier l'état général du système. NOTE (bug-badalloc-ws-broadcast) : l'alerte
  // ci-dessous porte sur le heap TOTAL libre (ESP.getFreeHeap), métrique DISTINCTE
  // du plus gros bloc contigu — l'échantillonnage de maxAllocHeap et l'auto-guérison
  // vivent désormais dans checkHeapHealth() (timer adaptatif, source unique).
  // churn-heap Q4 : émission sur TRANSITION uniquement (pattern edge, comme
  // lastPhLimitReached) — avant, le CRITICAL (+ flush disque immédiat) et l'alerte
  // MQTT étaient RÉ-ÉMIS à chaque tick de 60 s tant que le heap restait sous le
  // seuil. Une alerte à l'entrée sous le seuil, une INFO au retour au-dessus.
  size_t freeHeap = ESP.getFreeHeap();
  static bool lastBelowMinFreeHeap = false;
  bool belowMinFreeHeap = (freeHeap < kMinFreeHeapBytes);
  if (belowMinFreeHeap && !lastBelowMinFreeHeap) {
    systemLogger.critical("Mémoire faible: " + String(freeHeap) + " bytes");
    mqttManager.publishAlert("low_memory", "Free heap: " + String(freeHeap) + " bytes");
  } else if (!belowMinFreeHeap && lastBelowMinFreeHeap) {
    systemLogger.info("Mémoire rétablie: " + String(freeHeap) + " bytes");
  }
  lastBelowMinFreeHeap = belowMinFreeHeap;

  // ===== Watchdog de connectivité (v2.9.2) =====
  // La pile lwIP peut se figer (thread tcpip NON surveillé par le WDT) : web/mDNS/ICMP
  // morts alors que loopTask tourne et que le heap est SAIN → ni le WDT ni l'auto-guérison
  // heap ne l'attrapent (gel du 2026-07-15, maxBloc ~37 Ko). Signe de vie PRIMAIRE : une
  // sonde ICMP active vers la passerelle (indépendante de MQTT). Décision au module pur.
  static bool softRecoverAttempted = false;
  if (WiFi.isConnected()) {
    ensureNetworkProbe();  // (re)démarre la sonde ICMP persistante (idempotent)
  } else {
    // WiFi absent : gel de l'horloge de stall (la perte WiFi est traitée plus bas par la
    // logique de reconnexion/AP secours, pas par ce watchdog) + réarmement du palier soft.
    lastNetworkOkMs = millis();
    softRecoverAttempted = false;
  }

  // Garde OTA (v2.9.2) : idem bloc heap — aucun reboot/soft-recover pendant un flash.
  if (!maintenanceMode && !PumpController.isOtaInProgress()) {
    uint32_t msSinceNetOk = (uint32_t)(millis() - lastNetworkOkMs);
    // Réseau redevenu sain : réarme le palier soft pour un futur épisode.
    if (msSinceNetOk < kNetworkStallSoftMs) softRecoverAttempted = false;

    NetHealthInputs nhIn;
    nhIn.wifiAssociated = WiFi.isConnected();
    nhIn.msSinceLastNetOk = msSinceNetOk;
    nhIn.softStallMs = kNetworkStallSoftMs;
    nhIn.hardStallMs = kNetworkStallRebootMs;
    nhIn.softRecoverAttempted = softRecoverAttempted;
    nhIn.uptimeMs = (uint32_t)millis();
    nhIn.minUptimeMs = kAutoHealMinUptimeMs;
    nhIn.rebootCountInWindow = rtcAutoHealRebootCount;
    nhIn.maxReboots = kAutoHealMaxReboots;

    NetHealthDecision netDecision = evaluateNetHealth(nhIn);
    if (netDecision == NetHealthDecision::Restart) {
      performFailSafeRestart(
          "Watchdog connectivité : pile réseau figée, redémarrage propre",
          "network_stall_reboot",
          "Pile réseau muette " + String(msSinceNetOk / 1000UL) +
          "s (maxBloc sain) — redémarrage");  // ne retourne pas (ESP.restart)
    } else if (netDecision == NetHealthDecision::EnterMaintenance) {
      enterMaintenanceMode();
    } else if (netDecision == NetHealthDecision::SoftRecover) {
      systemLogger.warning("Watchdog connectivité : pile muette " +
          String(msSinceNetOk / 1000UL) + "s — reconnexion WiFi (récupération soft)");
      WiFi.disconnect(false);
      WiFi.reconnect();
      softRecoverAttempted = true;
      mqttManager.publishAlert("network_soft_recover",
          "Pile réseau muette — reconnexion WiFi tentée");
    }
  }

  // Ré-annonce mDNS demandée par le handler d'événement WiFi (changement d'IP suite à
  // un roaming CPL/mesh, ou reconnexion après déconnexion). Exécutée ici, dans le
  // contexte loopTask, pour éviter tout accès mDNS concurrent. Découplé du compteur
  // wifiReconnectAttempts : couvre aussi le roaming sans perte de connexion.
  if (mdnsReannounceRequested && WiFi.isConnected()) {
    // Debounce (v2.9.1) : ne ré-annoncer que si l'IP a RÉELLEMENT changé ET si
    // l'intervalle mini est écoulé — ignore les oscillations transitoires d'IP du
    // double NAT CPL qui provoquaient un churn MDNS.end()/begin() coûteux.
    IPAddress currentIp = WiFi.localIP();
    unsigned long nowMs = millis();
    if (currentIp == mdnsLastAnnouncedIp) {
      // Aucun changement réel d'IP : requête parasite (oscillation retombée) →
      // rien à ré-annoncer, on consomme le drapeau.
      mdnsReannounceRequested = false;
    } else if (lastMdnsReannounceMs == 0 ||
               nowMs - lastMdnsReannounceMs > kMdnsReannounceMinIntervalMs) {
      // IP réellement nouvelle ET intervalle mini écoulé : ré-annonce effective.
      // Le drapeau n'est consommé qu'APRÈS exécution.
      reannounceMdns("changement d'IP");
      lastMdnsReannounceMs = nowMs;
      mdnsReannounceRequested = false;
    } else {
      // IP nouvelle mais dans l'intervalle de debounce : on NE consomme PAS le
      // drapeau → la ré-annonce se fera au prochain tick une fois l'intervalle
      // écoulé, sans dépendre d'un futur événement WiFi.
      systemLogger.debug("Ré-annonce mDNS différée (debounce, IP=" + currentIp.toString() + ")");
    }
  }

  // Vérifier connexion WiFi et activer AP en secours si nécessaire
  static unsigned long lastWifiCheckTime = 0;
  static int wifiReconnectAttempts = 0;
  wifi_mode_t mode = WiFi.getMode();

  if (!WiFi.isConnected() && (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)) {
    unsigned long now = millis();

    // Première tentative ou si 30 secondes se sont écoulées depuis la dernière tentative
    if (lastWifiCheckTime == 0 || (now - lastWifiCheckTime >= 30000)) {
      // Le compteur (n/3) n'a de sens qu'en mode STA (il arme la bascule AP secours).
      // En AP+STA le secours est déjà actif et les tentatives continuent indéfiniment :
      // afficher un compteur non borné (« 47/3 ») serait faux.
      if (mode == WIFI_MODE_APSTA) {
        systemLogger.warning("WiFi déconnecté, tentative de reconnexion STA (AP secours actif)");
      } else {
        systemLogger.warning("WiFi déconnecté, tentative de reconnexion (" + String(wifiReconnectAttempts + 1) + "/3)");
      }
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
      // La ré-annonce mDNS n'est plus faite ici : le handler d'événement
      // ARDUINO_EVENT_WIFI_STA_GOT_IP pose mdnsReannounceRequested (après
      // invalidation sur DISCONNECTED), et la ré-annonce est exécutée en tête
      // de checkSystemHealth(). Cela couvre aussi le roaming sans perte de WiFi.
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

  // Alerte MQTT à la TRANSITION uniquement (v2.10.1 : publishAlert
  // était appelé sans condition → une alerte + un log WARN par health check de 60 s
  // tant que la limite restait latchée). L'état permanent pour HA est porté par le
  // binary_sensor ph_limit (retain) — l'alerte est un événement de transition.
  if (safetyLimits.phLimitReached && !lastPhLimitReached) {
    const char* phLabel = (mqttCfg.phCorrectionType == "ph_plus") ? "pH+" : "pH-";
    mqttManager.publishAlert("ph_limit", String("Limite journalière ") + phLabel + " atteinte");
  }
  lastPhLimitReached = safetyLimits.phLimitReached;

  if (safetyLimits.orpLimitReached && !lastOrpLimitReached) {
    mqttManager.publishAlert("orp_limit", "Limite journalière chlore atteinte");
  }
  lastOrpLimitReached = safetyLimits.orpLimitReached;

  // Vérifier valeurs capteurs aberrantes — log et alerte MQTT à la transition seulement
  // feature-067 : la détection porte sur le pH CORRIGÉ (valeur "vraie" du bassin),
  // mais le message cite TOUJOURS le brut EZO et l'offset appliqué — sans quoi on
  // ne peut pas distinguer une sonde à la dérive d'un offset mal saisi.
  float ph = sensors.getPh();        // corrigé (brut + offset)
  float phRaw = sensors.getPhRaw();  // brut EZO
  float phOffset = sensors.getPhOffset();
  float orp = sensors.getOrp();
  float temp = sensors.getTemperature();

  static bool lastPhAbnormal   = false;
  static bool lastOrpAbnormal  = false;
  static bool lastTempAbnormal = false;

  // feature-091 (C6 pool-chemistry) : hors porte de validité (purge, re-purge,
  // circulation arrêtée), l'eau de la conduite n'est PAS celle du bassin — une
  // valeur « anormale » y est attendue, pas alarmante. Les alertes pH/ORP ne
  // portent que sur des mesures représentatives ; la température n'est pas
  // concernée (sonde DS18B20 indépendante de la purge).
  const bool measuresValid = measureValidity.measuresValid();
  bool phAbnormal   = measuresValid && (ph < 5.0f || ph > 9.0f);
  bool orpAbnormal  = measuresValid && (orp < 400.0f || orp > 900.0f);
  bool tempAbnormal = (!isnan(temp) && (temp < 5.0f || temp > 40.0f));

  if (phAbnormal && !lastPhAbnormal) {
    // Message unique corrigé + brut + offset (feature-067) : réutilisé tel quel
    // par le log et par l'alerte MQTT.
    const String phDetail = "pH=" + String(ph, 2) + " (brut=" +
                            (isnan(phRaw) ? String("NaN") : String(phRaw, 2)) +
                            ", offset=" + String(phOffset, 2) + ")";
    if (authCfg.sensorLogsEnabled) systemLogger.warning("Valeur pH anormale: " + phDetail);
    mqttManager.publishAlert("ph_abnormal", phDetail);
  } else if (!phAbnormal && lastPhAbnormal) {
    if (authCfg.sensorLogsEnabled)
      systemLogger.info("Valeur pH revenue à la normale: " + String(ph, 2) + " (brut=" +
                        (isnan(phRaw) ? String("NaN") : String(phRaw, 2)) +
                        ", offset=" + String(phOffset, 2) + ")");
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

