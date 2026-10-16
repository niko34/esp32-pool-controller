#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>  // heap_caps_malloc_extmem_enable (bascule PSRAM, bug-080 B-1)
#include <esp_memory_utils.h>  // esp_ptr_external_ram (témoin de bascule PSRAM, feature-122 A7)
#include <soc/soc.h>           // SOC_EXTRAM_DATA_LOW/HIGH (fenêtre d'adressage PSRAM)
// 2e avis sur le camp d'un pointeur (témoin A7) : esp_psram_check_ptr_addr, de
// <esp_private/esp_psram_extram.h>. __has_include sert de test de DISPONIBILITÉ,
// mais le header n'est PAS inclus : il redéclare esp_psram_extram_test() en
// linkage C alors qu'Arduino.h:256 la déclare en linkage C++ — l'inclusion des
// deux ne compile pas. On déclare donc le seul symbole utilisé (présent dans
// libesp_psram.a, vérifié au nm).
#if __has_include(<esp_private/esp_psram_extram.h>)
#define POOL_HAS_PSRAM_PTR_CHECK 1
extern "C" bool esp_psram_check_ptr_addr(const void* p);
#else
#define POOL_HAS_PSRAM_PTR_CHECK 0
#endif
#include <Preferences.h>
#include <nvs_flash.h>
#include <atomic>  // bug-134 : netSelfRearmCount / netIcmpEchoCount, incrémentés depuis plusieurs tâches

#include "config.h"
#include "constants.h"
#include "logger.h"
#include "auth.h"
#include "sensors.h"
#include "ph_offset.h"       // feature-168 (C10) : isPhOffsetExpired() (logique pure)
#include "ph_guard_logic.h"  // feature-168 (C6+C5bis) : isPhRegulationImpasseLow/High()
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
#include "dosing_window.h"    // feature-136 : survie du budget horaire de dosage au reboot
#include "time_to_target.h"  // feature-096 : indicateur temps d'atteinte de cible
#include "version.h"
#include "rtc_manager.h"
#include "self_heal_logic.h"
#include "net_health_logic.h"
#include "heap_drop_logic.h"  // détecteur PUR de chute de contiguïté (feature-144, ADR-0017)
#include "heap_ring.h"
#include "fs_guard.h"     // bug-100 : drain de l'épisode de famine fichiers (ADR-0070)
#include "heap_metrics.h"  // mesure 8 bits (mémoire réellement allouable) — trace de calibrage v2.19.0
#include "mbedtls_psram_alloc.h"  // bug-165/bug-108 : mbedtls obéit à la politique PSRAM (v2.81.0)
#include "heap_census.h"  // feature-128 : recensement du tas (R2) + lecture de contenu (R3, sous POOL_HEAP_PEEK)
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
// fige. Recréée uniquement si la passerelle change (roaming subnet), ou de force sur
// demande du watchdog (bug-134, ensureNetworkProbe(force)). nullptr = absente.
// ÉCRIT exclusivement par loopTask (jamais d'autre tâche) : c'est ce qui rend
// la chaîne stop+delete+new sûre sans mutex. LU aussi depuis async_tcp en
// diagnostic (netProbeAliveGet(), `/get-system-info`) — lecture 32 bits alignée
// d'un simple booléen de présence, sans risque, mais l'invariant d'écriture
// exclusive ne s'étend pas à la lecture.
static esp_ping_handle_t netPingHandle = nullptr;
static IPAddress netProbeGateway;

// ===== Watchdog de connectivité — réarmement plafonné (bug-134) =====
// Crédit de réarmements AUTO-PRODUITS (IP DHCP obtenue, sonde ICMP relancée)
// consommé par evaluateNetRearm() (src/net_health_logic.h) via la porte unique
// notifyNetworkLifeSign(). std::atomic et NON volatile : lu/modifié/écrit
// (load → calcul pur → store, PAS de read-modify-write atomique — cf. revue)
// depuis plusieurs tâches (thread esp_ping, tâche événements WiFi, async_tcp,
// mqttTask, loopTask). Une course entre deux écrivains AUTO-PRODUITS (au plus
// deux existent : tâche événements WiFi pour IpAcquired, loopTask pour
// ProbeStarted) peut faire perdre UN incrément — les deux liraient alors la
// même valeur et s'arrêteraient au même plafond. Effet BORNÉ et dans le sens
// SÛR : au plus un réarmement auto-produit EN TROP, donc un report du reboot
// d'environ une fenêtre de silence — jamais un de moins, jamais de boucle
// rouverte. memory_order_relaxed : ce compteur ne publie aucune autre donnée,
// seul son propre ordre d'écriture compte.
static std::atomic<uint8_t> netSelfRearmCount{0};
// Dernier écho ICMP reçu (millis), indépendant de lastNetworkOkMs (que d'autres
// signaux réarment aussi) : sert à détecter la mort de la sonde elle-même (A3).
// volatile : écrit par le thread esp_ping, lu par loopTask.
volatile unsigned long lastIcmpEchoMs = 0;
// Nombre total d'échos ICMP reçus depuis le boot — observabilité (A3) et champ
// de diagnostic `/get-system-info`. std::atomic : incrémenté hors loopTask.
static std::atomic<uint32_t> netIcmpEchoCount{0};
// Demande de recréation FORCÉE de la sonde ICMP (bug-134), posée par le
// watchdog (soft recover, ou silence de sonde confirmé) et consommée au tick
// SUIVANT de checkSystemHealth(). Accès EXCLUSIF loopTask (posé et lu depuis le
// même contexte, jamais depuis le handler WiFi) : pas d'atomique nécessaire.
// ⚠️ Ne jamais appeler ensureNetworkProbe(true) directement depuis le handler
// WiFi : au moment du SoftRecover, WiFi vient d'être déconnecté et la fonction
// sortirait au premier `if (!WiFi.isConnected())`.
static bool netProbeRestartRequested = false;

// Déclaration des fonctions
bool setupWiFi();
void resetWiFiSettings();
void applyTimeConfig();
void checkSystemHealth();
void checkHeapHealth();
void onNtpTimeSync();
void notifyNetworkActivity();
void notifyNetworkLifeSign(NetLifeSign sign);
void reannounceMdns(const char* reason);
void initAutoHealRtcState();
void performFailSafeRestart(const char* reason, const char* alertType, const String& alertMsg);
void performAutoHealRestart(uint32_t maxAllocHeap);
void enterMaintenanceMode();
void ensureNetworkProbe(bool force = false);
void handleHeapCorruptionDetected();

void setup() {
  // Parachute OOM (bug-oom-accept-http-durcissement, crash du 11/08) : réserve
  // d'urgence allouée AU PLUS TÔT — avant Serial/logger/FS — pour prendre un
  // bloc contigu avant toute fragmentation. N'émet aucun log (drainé en loop).
  initOomGuard();

  // bug-080 lot B-1 : bascule des allocations ordinaires « de plus de
  // kPsramExtmemThresholdBytes octets » vers la PSRAM D'ABORD. Le seuil du
  // sdkconfig vaut 4096 et son test est INCLUSIF (« taille <= seuil ⇒ RAM
  // interne ») : le tampon stdio de 4096 o que la couche FS attache à CHAQUE
  // fichier LittleFS ouvert restait donc en RAM interne, à un octet près,
  // jusqu'à ~49 Ko réclamés en rafale par un simple chargement d'interface.
  // feature-124 (S7) : le seuil descend de 4 095 à 1 024 — la bascule cesse
  // d'être « chirurgicale » et devient un vrai déplacement de politique, pour
  // que les pbufs lwIP (~1 600 o) partent aussi en PSRAM. L'encadrement
  // [512 ; 1 599] et son motif sont écrits sur la constante (constants.h).
  // ⚠️ Ce site NE déplace AUCUN tampon WiFi : les allocations qui échouent
  // demandent `caps=0x080C` explicitement et contournent ce seuil. Il leur
  // fait de la place, il ne les corrige pas.
  // Placé JUSTE APRÈS initOomGuard : la réserve du parachute est déjà prise en
  // RAM interne, et tout le reste du setup profite de la nouvelle politique.
  // AUCUN LOG ICI (systemLogger.begin() n'a pas encore eu lieu) ; garde
  // psramFound() pour le binaire unique WROOM/WROVER-E.
  if (psramFound()) {
    heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes);

    // bug-165/bug-108 (v2.81.0) : mbedtls FORCE ses allocations en RAM INTERNE
    // (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1, sdkconfig des libs pioarduino
    // précompilées — non modifiable sans recompiler mbedtls) et consommait
    // ~56 Ko de tas interne pendant la vérification ECDSA P-384 d'un handshake
    // TLS sortant (maxAlloc8 mesuré à 3 316 o), alors que la PSRAM dormait.
    // Posé ICI, JUSTE APRÈS la politique d'extension PSRAM ci-dessus et AVANT
    // TOUTE utilisation de mbedtls (Wi-Fi compris, cf. setupWiFi() bien plus
    // bas) : aucun code de ce fichier n'invoque mbedtls entre ce point et
    // setupWiFi() — invariant déjà vrai avant ce lot, PRÉSERVÉ. AUCUN LOG ICI
    // (systemLogger.begin() n'a pas encore eu lieu) ; l'état est mémorisé dans
    // mbedtlsPsramAllocatorState() et journalisé plus bas, avec le reste des
    // témoins de bascule PSRAM.
    mbedtlsPsramAllocInstall();

    // feature-122 (A7) : LA BASCULE EST-ELLE DÉCORATIVE ? `psram_free` est
    // immobile à 4 180 340 dans 152 relevés — ce qui signifie soit que rien ne
    // part en PSRAM, soit que l'indicateur ne le voit pas. Seule preuve
    // DIRECTE : allouer et regarder l'adresse.
    //
    // DEUX allocations, libérées IMMÉDIATEMENT (aucune rétention, aucun risque),
    // exprimées RELATIVEMENT AU SEUIL (feature-124, condition C4 du gate — le
    // couple 4096/4095 devenait mensonger à seuil 1 024, où un malloc de 4 095 o
    // part en PSRAM) :
    //  - `seuil + 1` o = la PLUS PETITE classe que la politique doit déporter,
    //    ATTENDUE en PSRAM. La classe historique du tampon stdio VFS (4 096 o,
    //    cible de B-1) est couverte a fortiori : 4 096 > seuil, même branche ;
    //  - `seuil` o = témoin de CONTRÔLE, attendu INTERNE dans les deux
    //    configurations (test inclusif). Le couple donne une signature non
    //    ambiguë : si les deux tombent du même côté, la politique ne fait rien —
    //    et il garde ce pouvoir discriminant à TOUT seuil futur.
    //
    // ⚠️ `malloc()` et JAMAIS heap_caps_malloc() avec des caps explicites : des
    // caps explicites contourneraient la politique `extmem` qu'on cherche
    // justement à éprouver, et le témoin ne prouverait rien.
    // AUCUN LOG ICI (systemLogger.begin() n'a pas encore eu lieu) : le résultat
    // est MÉMORISÉ (heapWitnessRecord) puis journalisé par la trace agrégée de
    // checkHeapHealth() — idiome déjà en place pour le parachute OOM.
    static_assert(SOC_EXTRAM_DATA_LOW < SOC_EXTRAM_DATA_HIGH,
                  "Fenetre d'adressage de la RAM externe incoherente : le temoin A7 "
                  "classerait toutes les allocations du meme cote.");
#if CONFIG_IDF_TARGET_ESP32
    static_assert((uint32_t)SOC_EXTRAM_DATA_LOW == 0x3F800000u &&
                      (uint32_t)SOC_EXTRAM_DATA_HIGH == 0x3FC00000u,
                  "ESP32 classique : la fenetre PSRAM attendue est 0x3F800000-0x3FC00000. "
                  "Si l'IDF la deplace, le temoin A7 doit etre relu avant d'etre cru.");
#endif
    uint8_t witness = 0;
    void* probeAbove = malloc(kPsramExtmemThresholdBytes + 1);  // attendu PSRAM
    void* probeAt = malloc(kPsramExtmemThresholdBytes);         // attendu INTERNE (contrôle)
    if (probeAbove == nullptr) witness |= kHeapWitnessAboveFailed;
    if (probeAt == nullptr) witness |= kHeapWitnessAtThresholdFailed;
#if POOL_HAS_PSRAM_PTR_CHECK
    // Deux avis indépendants sur le camp d'un pointeur. Leur DISCORDANCE est un
    // résultat à rapporter (3ᵉ bit), pas une erreur à masquer : elle
    // signifierait que l'une des deux API ne dit pas ce qu'on croit — le genre
    // d'angle mort qui a produit les deux erreurs de métrique précédentes.
    bool disagree = false;
#else
    bool disagree = false;
    witness |= kHeapWitnessNoPsramApi;
#endif
    if (probeAbove != nullptr) {
      const bool ext = heapPtrIsExternal((uintptr_t)probeAbove, (uintptr_t)SOC_EXTRAM_DATA_LOW,
                                         (uintptr_t)SOC_EXTRAM_DATA_HIGH);
      if (ext) witness |= kHeapWitnessAboveExternal;
      if (ext != esp_ptr_external_ram(probeAbove)) disagree = true;
#if POOL_HAS_PSRAM_PTR_CHECK
      if (ext != esp_psram_check_ptr_addr(probeAbove)) disagree = true;
#endif
      free(probeAbove);
    }
    if (probeAt != nullptr) {
      const bool ext = heapPtrIsExternal((uintptr_t)probeAt, (uintptr_t)SOC_EXTRAM_DATA_LOW,
                                         (uintptr_t)SOC_EXTRAM_DATA_HIGH);
      if (ext) witness |= kHeapWitnessAtThresholdExternal;  // ANOMALIE : le contrôle a bougé
      if (ext != esp_ptr_external_ram(probeAt)) disagree = true;
#if POOL_HAS_PSRAM_PTR_CHECK
      if (ext != esp_psram_check_ptr_addr(probeAt)) disagree = true;
#endif
      free(probeAt);
    }
    if (disagree) witness |= kHeapWitnessDisagreement;
    heapWitnessRecord(witness);
  } else {
    // Module WROOM (binaire unique) : pas de PSRAM, le témoin n'a rien à
    // trancher. Marqué « sauté » plutôt que silencieusement nul.
    heapWitnessRecord(kHeapWitnessSkipped);
  }

  Serial.begin(115200);
  delay(kSerialInitDelayMs);

  systemLogger.begin();  // Initialise le mutex avant tout accès multi-core
  systemLogger.info("=== Démarrage ESP32 Pool Controller v" + String(FIRMWARE_VERSION) + " ===");
  // Armement à deux paliers (v2.18.8) : la taille RÉELLEMENT armée est
  // journalisée avec le maxBloc du moment (le palier de repli signale un tas
  // déjà contraint au boot).
  size_t oomArmedBytes = oomGuardReserveArmedBytes();
  if (oomArmedBytes > 0) {
    // Condition C1 du gate (bug-080 B-1) : l'EMPLACEMENT de la réserve est
    // journalisé, pas supposé. Depuis la bascule PSRAM, « la réserve est en RAM
    // interne » est un invariant qui doit s'observer — une réserve en PSRAM ne
    // sauverait rien d'une famine de RAM interne.
    systemLogger.info("Parachute OOM armé : réserve d'urgence de " +
                      String(oomArmedBytes) + " o (" +
                      String(oomArmedBytes >= kOomReserveBytes ? "palier nominal"
                                                              : "palier de repli") +
                      ") pré-allouée en " +
                      String(oomGuardReserveIsInternalRam() ? "RAM interne"
                                                            : "PSRAM (ANOMALIE)") +
                      // Métrique 8 bits : la même que les seuils du parachute (bug-080).
                      ", maxBloc8=" + String(heapLargestFreeBlock8Bit()) + " o");
  } else {
    // Improbable au boot : les deux mallocs refusés — la loop réarmera (hystérésis).
    systemLogger.warning("Parachute OOM : réserve non allouée au boot (nominal ET repli refusés) "
                         "— armement différé en loop");
  }
  systemLogger.info("Build: " + String(FIRMWARE_BUILD_DATE) + " " + String(FIRMWARE_BUILD_TIME));
  // feature-066 : détection PSRAM (module WROVER-E). psramFound() est fiable
  // ici : l'init a lieu avant setup() (BOARD_HAS_PSRAM) et échoue proprement
  // sur un module WROOM sans PSRAM (binaire unique, dégradation gracieuse).
  // bug-080 B-1 : ce message disait « les allocations > 4 Ko y sont placées
  // automatiquement ». C'était FAUX, et cet arrondi en kilo-octets a masqué le
  // défaut pendant trois cycles de diagnostic : le test de l'allocateur est
  // INCLUSIF (« taille <= seuil ⇒ RAM interne »), si bien qu'à seuil 4096 une
  // allocation de 4096 o — le tampon stdio de chaque fichier ouvert — restait en
  // RAM interne. Le seuil est désormais énoncé EN OCTETS, avec sa direction.
  if (psramFound()) {
    systemLogger.info("PSRAM : " + String(ESP.getPsramSize() / 1048576) + " Mo détectés, " +
                      String(ESP.getFreePsram() / 1024) + " Ko libres — allocations de PLUS de " +
                      String((unsigned long)kPsramExtmemThresholdBytes) +
                      " o tentées en PSRAM d'abord (repli RAM interne si refus) ; " +
                      String((unsigned long)kPsramExtmemThresholdBytes) +
                      " o et moins : RAM interne");
    // bug-165/bug-108 (v2.81.0) : état du crochet posé plus haut, dans le même
    // bloc psramFound(). Les trois états sont ÉCRITS EN TOUTES LETTRES (pas un
    // simple booléen) — le projet s'est déjà fait avoir une fois par un
    // parachute décoratif sans que rien ne le signale avant une mesure directe.
    switch (mbedtlsPsramAllocatorState()) {
      case kMbedtlsPsramAllocActive:
        systemLogger.info("mbedtls : allocateur calloc/free basculé vers la PSRAM "
                          "(témoin confirmé — bug-165/bug-108)");
        break;
      case kMbedtlsPsramAllocDecorative:
        systemLogger.critical("mbedtls : crochet PSRAM posé mais DÉCORATIF (témoin "
                              "tombé en RAM interne) — la famine du handshake TLS "
                              "persiste, voir bug-165/bug-108");
        break;
      default:
        systemLogger.critical("mbedtls : crochet PSRAM NON posé (mbedtls_platform_"
                              "set_calloc_free refusé) — la famine du handshake TLS "
                              "persiste, voir bug-165/bug-108");
        break;
    }
  } else {
    systemLogger.info("PSRAM absente — RAM interne seule (module WROOM), "
                      "politique d'allocation inchangée");
  }

  // Initialisation GPIO bouton factory reset et LED intégrée
  // PCB v2 (feature-019) : bouton factory reset sur GPIO35 (input-only,
  // sans pull-up interne possible) avec pull-up externe 10 kΩ vers 3V3 → actif bas.
  pinMode(kFactoryResetButtonPin, INPUT);
  pinMode(kBuiltinLedPin, OUTPUT);
  digitalWrite(kBuiltinLedPin, LOW);

  // Initialisation watchdog
  // feature-063 : IDF 5 configure le TWDT par une struct. Le TWDT est déjà
  // initialisé par le core Arduino → on RECONFIGURE (esp_task_wdt_init
  // renverrait ESP_ERR_INVALID_STATE).
  {
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = kWatchdogTimeoutSec * 1000,
        .idle_core_mask = 0,   // pas d'abonnement des tâches idle
        .trigger_panic = true, // panic → reset au timeout
    };
    esp_task_wdt_reconfigure(&wdtConfig);
  }
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
  // feature-140 : restaure l'état de la Chloration de secours (survit à un
  // reboot ; expiré au chargement si 48h dépassées et heure valide — condition
  // #6 du gate : sans heure valide, isRescueChlorineActive() reste fail-closed
  // sans qu'un reset ici soit nécessaire).
  loadRescueState();

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
  // ⚠️ feature-158 (condition 13 du gate pool-chemistry) : DOIT rester AVANT
  // filtration.begin() ci-dessous — c'est cet appel qui produit le verdict de
  // continuité que filtration.begin() consomme (measureValidity.bootContinuityVerdict()).
  // Si cet ordre est un jour inversé par erreur, le drapeau « verdict produit »
  // fait refuser la reprise et journalise un `critical` — jamais un report deviné.
  measureValidity.begin();

  // feature-136 (gate pool-chemistry du 2026-10-08) : résout l'état restauré du
  // budget horaire de dosage (T2 RAM RTC + T1 NVS, le plus défavorable des deux
  // retenu) AVANT PumpController.begin(), qui l'applique à DosingState via
  // applyRestoredDosingWindow(). APRÈS rtcManager.applyToSystem() : la voie T1
  // (NVS, ancrée sur l'epoch murale) a besoin d'une heure valide pour être
  // interprétée.
  dosingWindow.begin();

  PumpController.begin();
  // feature-091 : l'arming legacy du timer de stabilisation au boot (mode Powered)
  // est RETIRÉ — la stabilisation après reprise de circulation est portée par la
  // purge adaptative de measureValidity (plancher + stabilité de la mesure).
  // ⚠️ feature-158 (condition 13 du gate pool-chemistry) : DOIT rester APRÈS
  // measureValidity.begin() ci-dessus — filtration.begin() y consomme le
  // verdict de continuité (bootContinuityVerdict()) pour décider d'une reprise
  // de la durée de filtration. Un réordonnancement futur de setup() ne doit
  // JAMAIS rapprocher ces deux appels sans vérifier ce commentaire des deux côtés.
  filtration.begin();
  lighting.begin();

  // feature-096 : indicateur « temps d'atteinte de cible ». APRÈS
  // PumpController.begin() (qui appelle loadDailyCounters : le compteur monotone
  // de filtration doit être restauré avant tout calcul de minutes) et APRÈS
  // rtcManager.applyToSystem() (une journée sans heure valide n'est pas datable).
  timeToTarget.begin();

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

    // feature-168 (C12) : alerte MQTT différée — le drapeau et le log critical
    // ont déjà eu lieu dans loadMqttConfig() (config.cpp, avant WiFi/MQTT,
    // outQueue inexistante à ce stade), MÊME piège que l'alerte ci-dessus.
    if (mqttCfg.phOffsetBootUnconfirmed) {
      String signStr = (mqttCfg.phOffset >= 0.0f) ? "+" : "";
      String offsetStr = signStr + String(mqttCfg.phOffset, 2); offsetStr.replace('.', ',');
      String unusualStr(kPhOffsetUnusualAbs, 2); unusualStr.replace('.', ',');
      mqttManager.publishAlert("ph_offset_stale",
        "[pH] Correction de mesure de " + offsetStr + " relue en mémoire au "
        "démarrage (au-delà de " + unusualStr + "). Elle est appliquée telle "
        "quelle — rien ne la borne. Confirmez-la ou corrigez-la : une valeur "
        "que vous n'avez pas saisie signale une mémoire altérée.");
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
  // feature-096 : indicateur passif (cadence 1 s en interne). APRÈS
  // measureValidity.update() (il lit la porte de mesure du tour COURANT) et
  // donc avant filtration.update() : il ne s'intercale PAS entre
  // filtration.update() et PumpController.update(), qui doivent voir le même
  // tour. Conséquence assumée et documentée (C5) : la grandeur de décision pH
  // lue ici est celle du tour PRÉCÉDENT — une seconde de retard sur une
  // métrique qui compte des minutes.
  timeToTarget.tick();
  history.update();
  systemLogger.update();
  // bug-100 / ADR-0070 : bornage du journal de la garde d'admission des fopen
  // internes. Placé juste APRÈS les deux producteurs de refus (historique et
  // journal), dans la loop EXISTANTE — aucune tâche FreeRTOS neuve.
  fsGuardDrain();
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
  //
  // feature-094 (revue, point n°2) : isPhDosing()/isOrpDosing() RETIRÉS de la
  // conjonction. Depuis que le verrou de session suit la PORTE et non le débit,
  // isPhDosing() reste vrai pompe à l'arrêt pendant toute une prolongation —
  // jusqu'à kPhSessionMaxWallMs (3 h). Les garder ici aurait rendu le détecteur
  // de corruption du tas AVEUGLE des heures d'affilée, alors que ce projet a
  // déjà connu une boot-loop lfs_alloc et un parachute jamais réarmé.
  // Ce qu'il faut protéger, c'est la CADENCE DES IMPULSIONS pendant qu'une pompe
  // tourne : isAnyPumpEnergized() le couvre exactement (et couvre en plus le
  // test manuel armé depuis la tâche web, invisible de isPh/isOrpDosing).
  // Une porte ouverte pompe à l'arrêt n'a rien à protéger.
  if (!maintenanceMode && !PumpController.isOtaInProgress() &&
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

  // Recensement du tas (feature-128, R2/R3) : consomme REQUESTED -> READY,
  // SEUL site du projet qui appelle heap_caps_walk pour ce module. Appelée à
  // CHAQUE tour de loop (contrairement au ring 2 s ci-dessus) : coût nul en
  // l'absence de requête (quelques lectures atomiques), et la latence de
  // réponse à une requête HTTP de diagnostic en dépend directement.
  heapCensusServiceLoop();

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
        // bug-134 (défaut n°1) : une IP obtenue par DHCP est une preuve de vie
        // L2+L3 irréfutable — mais AUTO-PRODUITE par la récupération elle-même,
        // donc plafonnée (evaluateNetRearm, NetLifeSign::IpAcquired) pour ne pas
        // ouvrir la boucle A (GOT_IP → réarme → soft recover → GOT_IP → ...).
        // Ce handler tourne sur la tâche événements système : UNE écriture 32
        // bits et un compteur, rien d'autre — pas de log, pas d'allocation, pas
        // de mutex.
        notifyNetworkLifeSign(NetLifeSign::IpAcquired);
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

// Porte UNIQUE de réarmement du watchdog de connectivité (bug-134). Applique
// evaluateNetRearm() (src/net_health_logic.h) pour distinguer les preuves de
// vie EXTERNES (IcmpEcho, AppTraffic — réarment sans limite ET remettent le
// crédit d'auto-réarmement à 0, condition C2) des preuves AUTO-PRODUITES par la
// récupération elle-même (IpAcquired, ProbeStarted — plafonnées à
// kNetworkSelfRearmMax, condition C1). Un signe de vie que la récupération
// fabrique elle-même ne peut pas financer indéfiniment son propre report — sans
// ce plafond, deux boucles infinies ont été démontrées (spec bug-134). Appelable
// depuis n'importe quelle tâche : écriture 32 bits atomique sur lastNetworkOkMs,
// netSelfRearmCount est un std::atomic<uint8_t> (relaxed).
// ⚠️ Cette porte n'est PAS absolument unique : checkSystemHealth() écrit aussi
// directement `lastNetworkOkMs = millis()` quand le WiFi est absent (gel
// préexistant de l'horloge, hors du champ de bug-134). C'est SANS RISQUE — cette
// branche ne remet jamais `netSelfRearmCount` à 0, et evaluateNetHealth()
// renvoie Continue tant que `!wifiAssociated` — mais le lecteur suivant doit le
// savoir plutôt que le déduire.
void notifyNetworkLifeSign(NetLifeSign sign) {
  NetRearmInputs in;
  in.sign = sign;
  in.selfRearmCount = netSelfRearmCount.load(std::memory_order_relaxed);
  in.maxSelfRearms = kNetworkSelfRearmMax;
  const NetRearmOutcome out = evaluateNetRearm(in);
  netSelfRearmCount.store(out.newSelfRearmCount, std::memory_order_relaxed);
  if (out.rearmClock) {
    lastNetworkOkMs = millis();
  }
}

// Point d'entrée public (v2.9.2) : signale une activité réseau réussie depuis
// n'importe quel module (MQTT ACK, requête HTTP/WS servie, resync NTP). Preuve
// EXTERNE (AppTraffic) : réarme sans limite et remet le crédit d'auto-réarmement
// à 0 via notifyNetworkLifeSign() (bug-134). Signature INCHANGÉE.
void notifyNetworkActivity() {
  notifyNetworkLifeSign(NetLifeSign::AppTraffic);
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
// loopTask : ne fait QUE poser l'horodatage, incrémenter le compteur d'échos (A3) et
// réarmer via la porte unique (preuve EXTERNE, bug-134) — rien d'autre.
static void onNetPingSuccess(esp_ping_handle_t /*hdl*/, void* /*args*/) {
  lastIcmpEchoMs = millis();
  netIcmpEchoCount.fetch_add(1, std::memory_order_relaxed);
  notifyNetworkLifeSign(NetLifeSign::IcmpEcho);
}

// (Re)démarre la sonde ICMP persistante vers la passerelle (v2.9.2). Idempotent :
// ne fait rien si la session existe déjà pour la passerelle courante — SAUF si
// `force` est vrai (bug-134 A2 : après un soft recover, ou un silence de sonde
// confirmé, la coquille lève cette garde pour réparer le signal primaire, même
// si la passerelle n'a pas changé). Recrée proprement (stop + delete) dans les
// deux cas, pour éviter toute fuite de session esp_ping.
// À appeler UNIQUEMENT depuis checkSystemHealth() (contexte loopTask), WiFi associé.
void ensureNetworkProbe(bool force) {
  if (!WiFi.isConnected()) return;
  IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress((uint32_t)0)) return;  // passerelle pas encore connue (DHCP)
  if (!force && netPingHandle != nullptr && gw == netProbeGateway) return;  // déjà en place

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
    // Réarme le témoin de silence de la sonde (A3) pour ne pas la déclarer morte
    // avant même d'avoir reçu son premier écho après ce (re)démarrage.
    lastIcmpEchoMs = millis();
    // bug-134 (C1, condition BLOQUANTE du gate pool-chemistry) : ProbeStarted est
    // une preuve AUTO-PRODUITE par la récupération elle-même — elle passe donc
    // TOUJOURS par le crédit plafonné, JAMAIS de réarmement inconditionnel. Sans
    // cette règle, une passerelle morte mais WiFi associé referme la boucle B :
    // silence 90s → recréation → réarmement → silence 90s → ... et le compteur
    // ne dépasse jamais 90s.
    notifyNetworkLifeSign(NetLifeSign::ProbeStarted);
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

// Accesseurs de diagnostic du watchdog de connectivité (bug-134), exposés par
// `/get-system-info` (src/web_routes_config.cpp). Lectures d'atomiques/volatile
// UNIQUEMENT : ni allocation, ni log, ni parcours de tas — sûr dans un handler
// AsyncWebServer.
uint32_t netIcmpEchoCountGet() {
  return netIcmpEchoCount.load(std::memory_order_relaxed);
}
uint32_t netMsSinceLastEchoGet() {
  return (uint32_t)(millis() - lastIcmpEchoMs);
}
uint32_t netMsSinceNetOkGet() {
  return (uint32_t)(millis() - lastNetworkOkMs);
}
uint8_t netSelfRearmCountGet() {
  return netSelfRearmCount.load(std::memory_order_relaxed);
}
bool netProbeAliveGet() {
  return netPingHandle != nullptr;
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
  // debounce). SOURCE UNIQUE, INTOUCHÉE par feature-144 : le détecteur de chute
  // ci-dessous CONSOMME cet échantillon, il n'en prend pas un second (les
  // heap_caps_* s'exécutent interruptions coupées — une seule mesure par tick).
  // bug-080 (v2.33.0) : point de mesure UNIQUE basculé sur la métrique 8 bits
  // RÉELLE. Ce même échantillon alimente le WARN+flush, le débounce auto-heal,
  // SelfHealInputs::maxAllocHeap et la cadence adaptative (lastHeapMaxAllocSample).
  uint32_t maxAllocHeap = heapLargestFreeBlock8Bit();
  // Export de l'échantillon pour la cadence adaptative du timer dans loop()
  // (bug-badalloc-ws-broadcast) — seule ligne ajoutée au bloc déplacé.
  lastHeapMaxAllocSample = maxAllocHeap;

  // ===== Détecteur de CHUTE de contiguïté (feature-144, v2.60.0) =====
  // Remet en service le détecteur d'ÉVÉNEMENT éteint le 2026-07-23 avec la
  // cadence périodique — celle-ci RESTE désactivée (c'est elle, et elle seule,
  // qui motivait l'extinction : ~288 lignes/j sur un journal persistant de
  // 8 Ko, Logger::kMaxLogFileBytes). Décision PURE déléguée à heap_drop_logic.h
  // (ADR-0017, seuil/réarmement/anti-rafale justifiés là-bas) : ce bloc ne fait
  // que lire des valeurs DÉJÀ mesurées et composer la ligne sur la PILE.
  // AUCUNE émission périodique, sous AUCUNE forme : en régime normal, zéro
  // ligne de plus qu'aujourd'hui — une ligne n'apparaît que sur une vraie chute.
  static HeapDropState sHeapDropState;
  HeapDropResult dropResult = heapDropEvaluate(
      sHeapDropState, maxAllocHeap, millis(), kHeapDropFloorBytes,
      kHeapDropRefShift, kHeapDropMinIntervalMs);
  if (dropResult.emit) {
    // libre8 : DERNIER relevé du masque 0x0804 (référence des gardes), déjà
    // tenu à jour par heapRingSample() (cadence 2 s) — lecture d'un atomique,
    // AUCUNE mesure de tas supplémentaire (invariant A3). ctx : idem, via
    // l'accesseur dédié de heap_ring.h — jamais recalculé ici (le bit HTTP est
    // un delta consommateur, cf. heap_ring.cpp).
    uint32_t libre8 = heapFloorsRead().masks[kHeapFloorSlotInternal8].lastFree;
    // ADR-0082 : snprintf sur tampon de PILE, aucune String, y compris quand
    // rien n'est émis (le snprintf est À L'INTÉRIEUR du `if`). Pire cas
    // ADR-0073 : 39 littéraux + 4×10 chiffres (%u) + 2 (%02X, uint8_t borné
    // 0-255) + NUL = 82 ⇒ 96 o, marge 14.
    // ⚠️ Nom de tampon DISTINCT de `ligne` (réservé à la ligne historique
    // « Heap 8bits: » plus bas, série de référence — cf.
    // test_native_heap_floors::test_the_historic_heap_8bits_line_is_untouched,
    // qui identifie son snprintf par le PREMIER tampon nommé `ligne` du fichier).
    char ligneChute[96];
    snprintf(ligneChute, sizeof(ligneChute),
             "Chute maxBloc8: %u -> %u (-%u) libre8=%u ctx=0x%02X",
             (unsigned)dropResult.before, (unsigned)dropResult.after,
             (unsigned)dropResult.dropAmount, (unsigned)libre8,
             (unsigned)heapRingLastCtx());
    systemLogger.info(ligneChute);
  }

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
    // « psram= » (bug-080 B-1) : la prédiction falsifiable du correctif est que
    // la PSRAM libre CHUTE à chaque chargement d'interface — sans ce chiffre
    // dans la même ligne que libre8, la corrélation n'est pas observable.
    // Appel LOCAL à ESP.getFreePsram() : surtout PAS un champ de HeapMetrics —
    // heapMetricsSnapshot() est appelé par WsManager::_heapAllowsPush() à chaque
    // trame, et les heap_caps_* s'exécutent interruptions coupées.
    // Tampon de PILE conservé (pire cas ~117 caractères sur 128 avec le nouveau
    // champ, en supposant les 6 valeurs à 10 chiffres — donc inatteignable en
    // pratique) : aucune String ici, cette trace s'émet PENDANT une pénurie.
    char ligne[128];
    snprintf(ligne, sizeof(ligne),
             "Heap 8bits: libre8=%u maxBloc8=%u minBoot=%u minFen=%u ws=%u psram=%u",
             (unsigned)m.free8, (unsigned)m.maxAlloc8, (unsigned)minBoot,
             (unsigned)minWindow, (unsigned)wsManager.clientCount(),
             (unsigned)ESP.getFreePsram());
    systemLogger.info(ligne);

    // feature-122 (A1/A5/A6/A7) — DEUX lignes agrégées, à la même cadence.
    //
    // POURQUOI DE NOUVELLES LIGNES ET PAS L'ÉLARGISSEMENT DE LA PREMIÈRE : le
    // tampon ci-dessus est à ~117 caractères sur 128 dans son pire cas, et la
    // ligne « Heap 8bits: » est la SEULE série longue dont dispose ce projet
    // (152 relevés historiques). Recalculer un pire cas à dix champs risquerait
    // de la tronquer silencieusement — on ne touche pas à la série de référence.
    //
    // POURQUOI LE JOURNAL PORTE L'ESSENTIEL DU RELEVÉ, ET PAS LA ROUTE HTTP —
    // ARBITRAGE, à ne pas défaire : `/get-system-info` sérialise sa réponse dans
    // une String (web_helpers.cpp), donc réclame ~1,7 Ko CONTIGUS sur le pool
    // 8 bits une fois enrichie. C'est la route qu'on interroge PENDANT une
    // famine, et le pire `maxBloc` observé descend à 852 o : il existe donc une
    // bande de fragmentation où elle échouerait alors qu'elle passait avant (le
    // 08/09 elle répondait en 200 quand la page statique était en 503). Ces
    // deux lignes, elles, sont des snprintf sur PILE : elles n'exigent AUCUN
    // bloc contigu du tas. Le journal est le chemin de dernier recours — d'où
    // le fait qu'il porte les PLANCHERS (et le contexte du creux), et non un
    // instantané qui ne dirait rien du régime de famine.
    // ADR-0033 : deux lignes par période de 30 min (~+0,55 Ko/h), JAMAIS une par
    // échantillon — le journal persistant n'a que ~25-35 min de rétention.
    //
    // LECTURE DES LIGNES :
    //  - « boot » = planchers DEPUIS LE DÉMARRAGE, « fen » = depuis le dernier
    //    report (réarmés ci-dessous) ; format « libre/plus gros bloc » ;
    //  - m804 = métrique actuelle des gardes, m80C = classe d'échec observée en
    //    production, m1804 = candidate du lot C-1b, mPS = PSRAM (A6 : même
    //    chiffre que `psram_free`, mais en MINIMUM — une allocation PSRAM
    //    transitoire est invisible sur un instantané) ;
    //  - c=0x.. / ctxVu : bits kHeapCtx* (1=clients WS, 2=GET HTTP, 4=dosage,
    //    8=rafale de découverte HA) au creux du masque, puis tous ceux vus dans
    //    la fenêtre — c'est ce qui dit QUEL événement a produit le plancher ;
    //  - tem = témoin de bascule PSRAM (bits kHeapWitness*, RELATIFS AU SEUIL
    //    depuis feature-124 : 2=le `seuil + 1` o est en PSRAM (attendu),
    //    4=le `seuil` o aussi (ANOMALIE), 8=discordance des deux API,
    //    1=sauté) ; positions de bits inchangées, `w` reste comparable ;
    //  - walk=max/med µs = durée OBSERVÉE du parcours 0x0804 (condition 5 du
    //    gate ; médiane approchée par borne supérieure de seau, donc
    //    surestimée) — à comparer aux 300 µs de kOomProbeWarnDurationUs. Ce
    //    n'est PAS `oom_probe_max_us`, qui mesure la sonde du chemin d'échec ;
    //  - b804 (ligne3 seulement) = DERNIER relevé libre/blocs alloués de la
    //    référence 0x0804 — pas un plancher, un instantané (fragmentation-v2500) ;
    //  - 4294967295 = aucun échantillon pour ce plancher.
    // Le « plus gros bloc » de 0x1000 et de la PSRAM n'est jamais affiché : ces
    // masques empruntent le chemin O(régions), leur bloc n'est pas mesuré.
    const HeapFloors hf = heapFloorsRead();
    const HeapFloorEntry& f804 = hf.masks[kHeapFloorSlotInternal8];
    const HeapFloorEntry& f80C = hf.masks[kHeapFloorSlotInternalDma8];
    const HeapFloorEntry& f1804 = hf.masks[kHeapFloorSlotInternal8Default];
    const HeapFloorEntry& fPS = hf.masks[kHeapFloorSlotSpiram];
    // Pire cas mesuré : 143 caractères sur 160 (six valeurs à 10 chiffres, ce
    // qu'aucune ne peut atteindre — le tas fait 4,2 Mo au plus).
    char ligne2[160];
    snprintf(ligne2, sizeof(ligne2),
             "Heap masq boot: m804=%u/%u c=0x%02X m80C=%u/%u c=0x%02X "
             "m1804=%u/%u c=0x%02X mPS=%u tem=0x%02X",
             (unsigned)f804.minFreeSinceBoot, (unsigned)f804.minLargestSinceBoot,
             (unsigned)f804.ctxAtMinLargest,
             (unsigned)f80C.minFreeSinceBoot, (unsigned)f80C.minLargestSinceBoot,
             (unsigned)f80C.ctxAtMinLargest,
             (unsigned)f1804.minFreeSinceBoot, (unsigned)f1804.minLargestSinceBoot,
             (unsigned)f1804.ctxAtMinLargest,
             (unsigned)fPS.minFreeSinceBoot, (unsigned)hf.witness);
    systemLogger.info(ligne2);

    // free_blocks/allocated_blocks (instrumentation fragmentation,
    // bug fragmentation-v2500) : DERNIER relevé de la RÉFÉRENCE 0x0804
    // seulement — c'est le seul masque parcouru à CHAQUE échantillon (les
    // autres n'ont ce compte qu'au tour de rôle, 4-8 s), et c'est la métrique
    // des gardes du projet. `libre8/b804` donne la taille MOYENNE d'un trou :
    // c'est la grandeur qui manquait pour distinguer un morcellement modéré
    // (peu de gros trous) d'une pulvérisation (beaucoup de petits), à
    // free8 constant. Pas de plancher tenu pour ces deux compteurs (ce lot
    // n'en demande pas) : c'est un DERNIER relevé, comme `f804.lastFree`.
    // Budget : MÊME MÉTHODE mécanique que ligne2 (ADR-0073 — pas de 2e méthode
    // de budget dans le même test) : chaque `%u` compte pour 10 chiffres
    // (4294967295) depuis l'origine, sans exception pour b804, même si un
    // compte de blocs est en pratique borné bien plus bas (taille du tas /
    // taille mini d'un bloc TLSF). Pire cas mesuré : 163 caractères sur 176
    // (10 valeurs %u à 10 chiffres + 1 octet de contexte en %02X + littéraux).
    // 0x0804 est TOUJOURS la branche heap_caps_get_info (jamais le chemin
    // O(régions), cf. static_assert kHeapMaskInternal8) : jamais de sentinelle
    // kOomProbeBlockCountUnknown à gérer ici.
    char ligne3[176];
    snprintf(ligne3, sizeof(ligne3),
             "Heap masq fen: m804=%u/%u m80C=%u/%u m1804=%u/%u ctxVu=0x%02X "
             "walk=%u/%uus b804=%u/%u",
             (unsigned)f804.minFreeWindow, (unsigned)f804.minLargestWindow,
             (unsigned)f80C.minFreeWindow, (unsigned)f80C.minLargestWindow,
             (unsigned)f1804.minFreeWindow, (unsigned)f1804.minLargestWindow,
             (unsigned)hf.ctxSeen,
             (unsigned)hf.walkMaxUs, (unsigned)hf.walkMedianUs,
             (unsigned)f804.lastFreeBlocks, (unsigned)f804.lastAllocatedBlocks);
    systemLogger.info(ligne3);

    // C4 (gate pool-chemistry, feature-128) : WARN one-shot si le recensement
    // du tas (R2/R3) s'est auto-désactivé DÉFINITIVEMENT (C1, un parcours a
    // dépassé kHeapCensusMaxDurationUs). Ce site est HORS CHEMIN DE REQUÊTE
    // HTTP — c'est la loop, la même trace périodique que ci-dessus — donc
    // COMPATIBLE avec S13 : S13 interdit systemLogger sur les chemins
    // d'erreur du protocole HTTP (route ET consommation REQUESTED->READY de
    // heap_census.cpp), pas sur un événement d'auto-désactivation émis depuis
    // la loop. Sans ce WARN, l'opérant n'apprend la désactivation qu'en
    // essuyant un 410 côté route, sans savoir DE COMBIEN le seuil a été
    // franchi — alors que C1 existe précisément parce que la base dérive
    // (wmax +38 % en six jours, cf. constants.h).
    static bool heapCensusDisabledWarned = false;
    if (!heapCensusDisabledWarned && heapCensusIsDisabled()) {
      heapCensusDisabledWarned = true;
      char ligneCensus[128];
      snprintf(ligneCensus, sizeof(ligneCensus),
               "Recensement tas (feature-128) DESACTIVE definitivement : "
               "dernier=%uus pire=%uus seuil=%uus",
               (unsigned)heapCensusLastWalkUs(), (unsigned)heapCensusMaxWalkUs(),
               (unsigned)kHeapCensusMaxDurationUs);
      systemLogger.warning(ligneCensus);
    }

    // Réarmement de la fenêtre : même tâche que l'échantillonnage (loop), même
    // course bénigne documentée que heapTakeMinMaxAlloc8Window — un échantillon
    // pris entre la lecture et le réarmement part dans la fenêtre SUIVANTE et
    // reste, dans tous les cas, retenu par les planchers depuis-boot. Le
    // chronométrage du parcours n'est PAS réarmé : sa médiane gagne en sens avec
    // le nombre d'échantillons.
    heapFloorsRearmWindow();
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

  // ===== Watchdog de connectivité (v2.9.2, réarmement plafonné bug-134) =====
  // La pile lwIP peut se figer (thread tcpip NON surveillé par le WDT) : web/mDNS/ICMP
  // morts alors que loopTask tourne et que le heap est SAIN → ni le WDT ni l'auto-guérison
  // heap ne l'attrapent (gel du 2026-07-15, maxBloc ~37 Ko). Signe de vie PRIMAIRE : une
  // sonde ICMP active vers la passerelle (indépendante de MQTT). Décision au module pur.
  static bool softRecoverAttempted = false;
  if (WiFi.isConnected()) {
    // bug-134 (A2) : `netProbeRestartRequested` est un DROIT CONSOMMÉ, posé au
    // tick PRÉCÉDENT (SoftRecover, ou silence de sonde confirmé plus bas) —
    // jamais posé ici pour un usage immédiat, et jamais forcé depuis le handler
    // WiFi (WiFi y était encore déconnecté au moment du SoftRecover).
    ensureNetworkProbe(netProbeRestartRequested);
    netProbeRestartRequested = false;
  } else {
    // WiFi absent : gel de l'horloge de stall (la perte WiFi est traitée plus bas par la
    // logique de reconnexion/AP secours, pas par ce watchdog) + réarmement du palier soft.
    lastNetworkOkMs = millis();
    softRecoverAttempted = false;
  }

  // Observabilité de la sonde ICMP (A3) : trace SUR TRANSITION uniquement (comme
  // lastBelowMinFreeHeap ci-dessus) — jamais une ligne par ping (journal de 8 Ko,
  // rotation à 6 Ko). netProbeDeclaredDead mémorise l'état précédent. C'est aussi
  // ici que le silence de sonde (défaut n°2 : passerelle morte, WiFi associé)
  // demande sa propre recréation forcée, indépendamment d'un éventuel SoftRecover.
  //
  // ⚠️ REVUE (bloquant 1) : ensureNetworkProbe() réarme ELLE-MÊME lastIcmpEchoMs
  // dès qu'elle (re)crée une session (:1073), SANS qu'aucun écho réel ne soit
  // arrivé. Un simple `!netProbeSeemsDead(...)` retomberait donc à `false` juste
  // après la recréation forcée ci-dessous, et la latch se lèverait en journalisant
  // « rétablie » à tort — avec N inchangé — avant de se re-déclarer morte 90 s
  // plus tard (boucle ~210 s, ~1200 lignes/jour). On mémorise donc le compteur
  // d'échos AU MOMENT de la déclaration de mort, et on n'annonce « rétablie » que
  // si ce compteur a RÉELLEMENT progressé depuis — sinon on reste en état
  // « morte », en silence (une seule paire de lignes par épisode).
  static bool netProbeDeclaredDead = false;
  static uint32_t netProbeDeadEchoCount = 0;
  if (WiFi.isConnected() && netPingHandle != nullptr) {
    uint32_t msSinceLastEcho = (uint32_t)(millis() - lastIcmpEchoMs);
    uint32_t currentEchoCount = netIcmpEchoCount.load(std::memory_order_relaxed);
    bool probeDead = netProbeSeemsDead(msSinceLastEcho, kNetworkProbeSilenceMs);
    if (probeDead && !netProbeDeclaredDead) {
      systemLogger.warning("Sonde ICMP muette (" + String(msSinceLastEcho / 1000UL) +
          "s, " + String(msSinceLastEcho / kNetworkProbeIntervalMs) +
          " échos manqués) — recréation");
      netProbeRestartRequested = true;  // consommé au tick suivant (WiFi encore là)
      netProbeDeclaredDead = true;
      netProbeDeadEchoCount = currentEchoCount;
    } else if (netProbeDeclaredDead && currentEchoCount > netProbeDeadEchoCount) {
      // Preuve RÉELLE : au moins un écho reçu depuis la déclaration de mort
      // (pas seulement « le silence calculé est redevenu court » — ce que la
      // recréation elle-même produirait à tort, cf. note ci-dessus).
      systemLogger.info("Sonde ICMP rétablie (" + String(currentEchoCount) + " échos)");
      netProbeDeclaredDead = false;
    }
  }

  // Observabilité du crédit d'auto-réarmement (bug-134, bloquant 2 de la revue) :
  // l'épuisement du crédit est l'ÉVÉNEMENT QUI DÉCIDE LE REBOOT — le journal ne
  // doit pas en rester muet, sous peine de reproduire l'opacité du 19/09.
  // Tracé ICI (loopTask), PAS depuis notifyNetworkLifeSign() : elle tourne sur
  // la tâche événements WiFi et le thread esp_ping, et ne doit jamais
  // journaliser. Deux lignes MAXIMUM par épisode (edge, comme ci-dessus).
  static uint8_t netSelfRearmCountLastSeen = 0;
  uint8_t selfRearmCount = netSelfRearmCount.load(std::memory_order_relaxed);
  if (selfRearmCount != netSelfRearmCountLastSeen) {
    if (selfRearmCount > 0 && netSelfRearmCountLastSeen == 0) {
      systemLogger.warning("Watchdog connectivité : crédit auto-réarmement entamé (" +
          String(selfRearmCount) + "/" + String(kNetworkSelfRearmMax) + ")");
    }
    if (selfRearmCount >= kNetworkSelfRearmMax && netSelfRearmCountLastSeen < kNetworkSelfRearmMax) {
      systemLogger.warning(
          "Watchdog connectivité : crédit auto-réarmement épuisé — reboot imminent si le silence persiste");
    }
    netSelfRearmCountLastSeen = selfRearmCount;
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
      // bug-134 (défaut n°2) : répare le signal PRIMAIRE au retour du WiFi — la
      // sonde peut avoir été morte AVANT ce rebond (passerelle injoignable) et
      // rester idempotente-par-égalité-de-passerelle sans ce drapeau. Posé ici,
      // consommé au tick SUIVANT (WiFi encore déconnecté à cet instant précis).
      netProbeRestartRequested = true;
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

  // feature-168 (C6 + C5bis) — ZONE D'IMPASSE DE RÉGULATION : au-delà d'un
  // seuil de correction, le filet BRUT (fixe) mord STRUCTURELLEMENT avant que
  // la régulation corrigée n'atteigne son seuil chimique. L'appareil le DIT au
  // lieu de se fermer en silence (comportement attendu n°4 de la spec).
  // Patron edge-trigger EXACT de lastPhLimitReached ci-dessus : une alerte à
  // l'ENTRÉE en impasse seulement, AUCUN throttle temporel neuf (c'est ce
  // patron qui a réglé le spam de la limite journalière, v2.10.1).
  static bool lastPhImpasseLow  = false;
  static bool lastPhImpasseHigh = false;
  const bool isPhPlusProduct = (mqttCfg.phCorrectionType == "ph_plus");

  const bool phImpasseLow = !isPhPlusProduct && isPhRegulationImpasseLow(phOffset);
  if (phImpasseLow && !lastPhImpasseLow) {
    String signStr = (phOffset >= 0.0f) ? "+" : "";
    String offsetStr = signStr + String(phOffset, 2); offsetStr.replace('.', ',');
    String consigneStr(kPhTargetMin, 2); consigneStr.replace('.', ',');
    String brutStr(kPhTargetMin - phOffset, 2); brutStr.replace('.', ',');
    String floorStr(kPhRawFloorAbsolute, 2); floorStr.replace('.', ',');
    const String msg = "[pH] Impasse de régulation : avec une correction de " +
      offsetStr + ", la consigne " + consigneStr + " exige une mesure brute de " +
      brutStr + ", sous le refus d'acidifier fixé à " + floorStr + ". L'appareil "
      "cessera d'acidifier avant d'atteindre la consigne.";
    systemLogger.critical(msg);
    mqttManager.publishAlert("ph_impasse", msg);
  }
  lastPhImpasseLow = phImpasseLow;

  const bool phImpasseHigh = isPhPlusProduct && isPhRegulationImpasseHigh(phOffset);
  if (phImpasseHigh && !lastPhImpasseHigh) {
    String signStr = (phOffset >= 0.0f) ? "+" : "";
    String offsetStr = signStr + String(phOffset, 2); offsetStr.replace('.', ',');
    String plafondStr(kPhCeilingCorrected, 2); plafondStr.replace('.', ',');
    String brutStr(kPhCeilingCorrected - phOffset, 2); brutStr.replace('.', ',');
    String ceilingStr(kPhRawCeilingAbsolute, 2); ceilingStr.replace('.', ',');
    const String msg = "[pH] Impasse de régulation : avec une correction de " +
      offsetStr + ", le plafond " + plafondStr + " exige une mesure brute de " +
      brutStr + ", au-dessus du refus d'alcaliniser fixé à " + ceilingStr +
      ". L'appareil cessera d'alcaliniser avant d'atteindre le plafond.";
    systemLogger.critical(msg);
    mqttManager.publishAlert("ph_impasse", msg);
  }
  lastPhImpasseHigh = phImpasseHigh;

  // feature-168 (C10) — PÉREMPTION : une correction inhabituelle non
  // reconfirmée depuis plus de kPhOffsetConfirmIntervalDays jours. Même
  // patron edge-trigger ; niveau WARNING (non critique, non bloquant —
  // conforme à la décision utilisateur) à la différence de C6/C5bis/C7/C12.
  // ⚠️ Teste isPhOffsetExpired() SEULE (pas phOffsetStaleNow()) : les causes
  // « boot » (C12) et « calibration » (feature-156) ont CHACUNE déjà leur
  // propre alerte dédiée à leur propre instant de transition — ajouter ce
  // latch sur le OR des trois causes les aurait fait sonner une seconde fois.
  static bool lastPhOffsetExpired = false;
  const time_t nowForOffset = time(nullptr);
  const bool phOffsetExpired = isPhOffsetExpired(
      phOffset, mqttCfg.phOffsetEpoch,
      (nowForOffset >= kMinValidEpoch) ? (uint32_t)nowForOffset : 0,
      kPhOffsetConfirmIntervalDays, kPhOffsetUnusualAbs);
  if (phOffsetExpired && !lastPhOffsetExpired) {
    String offsetStr(phOffset, 2); offsetStr.replace('.', ',');
    const String msg = "[pH] Correction de mesure de " + offsetStr +
      " non revérifiée depuis plus de " + String(kPhOffsetConfirmIntervalDays) +
      " jours : voir l'écran de réglage pH.";
    systemLogger.warning(msg);
    mqttManager.publishAlert("ph_offset_stale", msg);
  }
  lastPhOffsetExpired = phOffsetExpired;

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

