#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>

struct MqttTopics {
  String base;
  String temperatureState;
  String temperatureCircuitState;  // feature-020 : T° circuit (2ᵉ sonde DS18B20)
  String phState;
  String orpState;
  String filtrationState;
  String filtrationModeState;
  String filtrationModeCommand;
  String filtrationCommand;    // ON/OFF switch command
  String lightingState;        // État éclairage
  String lightingCommand;      // ON/OFF switch command
  String phDosageState;
  String orpDosageState;
  String phDosingState;        // Pompe pH active (ON/OFF)
  String orpDosingState;       // Pompe ORP active (ON/OFF)
  String phLimitState;         // Limite journalière pH atteinte (ON/OFF)
  String orpLimitState;        // Limite journalière ORP atteinte (ON/OFF)
  String phStockLowState;      // Stock pH faible (ON/OFF)
  String orpStockLowState;     // Stock ORP faible (ON/OFF)
  String phRemainingState;     // Volume pH restant (ml)
  String orpRemainingState;    // Volume ORP restant (ml)
  String phTargetState;        // Consigne pH (état)
  String orpTargetState;       // Consigne ORP (état)
  String phTargetCommand;      // Consigne pH (commande)
  String orpTargetCommand;     // Consigne ORP (commande)
  String phRegulationModeState;    // Mode de régulation pH (automatic/scheduled/manual)
  String phDailyTargetMlState;     // Volume quotidien pH programmée (mL)
  String orpRegulationModeState;   // Mode de régulation ORP (automatic/scheduled/manual)
  String orpDailyTargetMlState;    // Volume quotidien ORP programmée (mL)
  String alertsTopic;
  String logsTopic;
  String statusTopic;          // LWT et status
  String diagnosticTopic;      // Diagnostic détaillé
  String alertsCalibrationTopic;  // feature-021 : alerte calibration EZO requise (retain)
  String alertsSensorStaleTopic;  // feature-021 : alerte capteur stale > kSensorStaleTimeoutMs
  String phCalPointsState;        // feature-021 : nb points calibration pH (Cal,?)
  String orpCalPointsState;       // feature-021 : nb points calibration ORP (Cal,?)
  String phSlopeAcidState;        // feature-024 : % pente acide pH (Slope,?)
  String phSlopeBaseState;        // feature-024 : % pente base pH (Slope,?)
  String phSlopeZeroState;        // feature-024 : décalage zéro pH (mV)
  // feature-025 : chaîne de filtrage pH/ORP (médiane + EMA + rejet pics)
  String phRawState;              // pH brut Atlas
  String phMedianState;           // pH médian
  String phFilteredState;         // pH filtré (valeur PID)
  String phFilterReadyState;      // ON/OFF filtre pH prêt
  String phFilterUnstableState;   // ON/OFF capteur pH instable
  String phRejectedCountState;    // nb rejets pH
  String orpRawState;
  String orpMedianState;
  String orpFilteredState;
  String orpFilterReadyState;
  String orpFilterUnstableState;
  String orpRejectedCountState;
  String phMixingDelayActiveState;  // ON/OFF pause mélange pH
  String orpMixingDelayActiveState; // ON/OFF pause mélange ORP
};

// Architecture producer/consumer (cf. ADR-0011) :
//
//   loopTask (core 1)                         mqttTask (core 0, prio 2, stack 8 KB)
//   ──────────────────                       ─────────────────────────────────────
//   publishXxx()       → outQueue ─────────→ drainOutQueue() → mqtt.publish()
//                                            mqtt.loop()      ← messageCallback()
//   drainCommandQueue() ← inQueue  ←──────── enqueueIncoming(cmd, payload)
//
// Aucun mqtt.publish() / mqtt.connect() / mqtt.loop() ne s'exécute jamais depuis
// loopTask. Inversement, mqttTask n'agit JAMAIS directement sur les actuateurs
// (filtration, lighting, pump_controller) — toute commande HA passe par inQueue
// pour être appliquée par loopTask sous configMutex.

class MqttManager {
private:
  WiFiClient wifiClient;
  PubSubClient mqtt;

  MqttTopics topics;
  bool discoveryPublished = false;

  // Reconnect/backoff — accédés UNIQUEMENT depuis mqttTask
  bool reconnectRequested = false;
  unsigned long lastAttempt = 0;
  unsigned long _reconnectDelay = 5000;  // Backoff exponentiel : 5s → 10s → 20s → ... → 120s max

  // Tâche dédiée MQTT
  TaskHandle_t taskHandle = nullptr;
  QueueHandle_t outQueue = nullptr;       // publish sortants (loopTask → mqttTask)
  QueueHandle_t inQueue = nullptr;        // commandes HA reçues (mqttTask → loopTask)
  std::atomic<bool> taskShouldStop{false};
  std::atomic<bool> connectedAtomic{false};

  // Drapeaux atomiques pour les publications périodiques (publishAllStates / publishDiagnostic)
  // Posés par loopTask via les méthodes publiques, consommés par mqttTask qui prend les
  // snapshots sous mutex puis enfile les publish individuels dans outQueue.
  std::atomic<bool> publishStatesRequested{false};
  std::atomic<bool> publishDiagnosticRequested{false};

  // Compteur de drops sur outQueue (logs WARN edge-triggered)
  uint32_t droppedSinceLastWarn = 0;
  unsigned long lastDropWarnMs = 0;

  void publishDiscovery();
  void refreshTopics();

  // feature-021 : caches pour publication edge-triggered de l'alerte calibration
  // et des états cal points. Lus/écrits uniquement depuis mqttTask.
  int  _lastPhCalPoints  = -2;  // -2 = jamais publié, -1..3 = valeurs réelles
  int  _lastOrpCalPoints = -2;
  bool _lastSensorStale  = false; // Cache pour alerte sensor_stale (NaN sur pH OU ORP)

  // feature-024 : caches pour publication edge-triggered des 3 topics pente.
  // Sentinelle NaN = "jamais publié" — la 1ʳᵉ query Slope,? réussie déclenchera
  // la publication initiale, puis chaque query réussie suivante (24h ou post-cal)
  // republiera les 3 topics. Lus/écrits uniquement depuis mqttTask.
  float _lastPhSlopeAcidPub = NAN;
  float _lastPhSlopeBasePub = NAN;
  float _lastPhSlopeZeroPub = NAN;

  // feature-025 : caches anti-spam pour les topics filtre pH/ORP. On ne republie
  // un topic retain que si la valeur (déjà arrondie) a changé. Lus/écrits uniquement
  // depuis mqttTask. Sentinelle vide = "jamais publié".
  String _lastFilterPub[14];
  // Publie `payload` sur `topic` uniquement si différent du dernier publié (slot `cacheIdx`).
  void safePublishDedup(int cacheIdx, const char* topic, const String& payload);
  // Publie l'ensemble des topics de la chaîne de filtrage (edge-triggered).
  void publishFilterStatesInternal();
  // Vérifie l'état de calibration et stale, publie/clear les alertes au besoin.
  // Appelé depuis mqttTask (publishAllStatesInternal + connectInTask).
  void publishCalibrationStatusInternal();

  // Internes — exécutées UNIQUEMENT depuis mqttTask
  static void mqttTaskFunction(void* pvParameters);
  void taskLoop();
  void drainOutQueue();
  void connectInTask();
  void publishAllStatesInternal();
  void publishDiagnosticInternal();

  // Wrapper unique pour tout mqtt.publish() depuis mqttTask. Voir feature-014 IT4 / ADR-0011.
  bool safePublish(const char* topic, const char* payload, bool retain);

  // Helpers de mise en file (depuis loopTask) — non-bloquants
  void enqueueOutbound(const String& topic, const String& payload, bool retain);
  void noteDropEdgeTriggered();

public:
  MqttManager();

  void begin();
  void update();   // No-op (legacy) — toute la logique vit dans mqttTask. Conservée pour compat des call sites.
  void connect();  // Délègue à mqttTask via reconnectRequested (préserve l'API existante)
  void disconnect();
  void messageCallback(char* topic, byte* payload, unsigned int length);

  // Drainage des commandes HA reçues — à appeler depuis loopTask à chaque tour de loop().
  // Les commandes (filtration/set, lighting/set, ph_target/set, orp_target/set) sont
  // appliquées sous configMutex. Aucun appel MQTT n'est effectué ici.
  void drainCommandQueue();

  // Arrêt propre avant ESP.restart() — publie status=offline synchronement avec timeout
  // puis termine la tâche. À appeler depuis web_server.cpp avant tout redémarrage planifié.
  void shutdownForRestart();

  bool isConnected() { return connectedAtomic.load(std::memory_order_relaxed); }
  void requestReconnect() { reconnectRequested = true; _reconnectDelay = 5000; lastAttempt = 0; }

  // Publication — API publique inchangée. Toutes ces méthodes sont des PRODUCTEURS
  // non-bloquants : elles enfilent un snapshot dans outQueue et retournent en < 1 ms.
  // Appelables depuis loopTask sans risque de blocage réseau.
  void publishSensorState(const String& topic, const String& payload, bool retain = true);
  void publishAllStates();
  void publishFiltrationState();
  void publishLightingState();
  void publishDosingState();
  void publishProductState();
  void publishTargetState();
  void publishAlert(const String& alertType, const String& message);
  void publishLog(const String& logMessage);
  void publishStatus(const String& status);
  void publishDiagnostic();

  // Getters
  const MqttTopics& getTopics() const { return topics; }
};

extern MqttManager mqttManager;

// Types internes échangés via les queues — exposés pour permettre aux
// utilitaires (debug, tests) d'inspecter la profondeur de file si besoin.
namespace mqtt_internal {

// Topics MQTT applicatifs : ~50c max ("pool/sensors/orp_regulation_mode" = 33c).
// Les topics discovery HA (~80c) ne passent JAMAIS par outQueue : ils sont publiés
// directement depuis mqttTask dans publishDiscovery().
constexpr size_t kMaxTopicLen   = 64;
// Payloads en queue sont courts : "ON"/"OFF", floats stringifiés (≤8c), JSON alerts (~80c).
// Le JSON diagnostic (~400c) ne passe PAS par outQueue : flag atomique → publish direct.
constexpr size_t kMaxPayloadLen = 128;

struct OutboundMsg {
  char topic[kMaxTopicLen];
  char payload[kMaxPayloadLen];
  bool retain;
};

enum class InboundCmdType : uint8_t {
  FiltrationMode,    // payload = "auto"|"manual"|"force"|"off"
  FiltrationOnOff,   // payload = "ON"|"OFF"
  Lighting,          // payload = "ON"|"OFF"
  PhTarget,          // payload = "7.2"
  OrpTarget,         // payload = "650"
};

struct InboundCmd {
  InboundCmdType type;
  char payload[64];
};

}  // namespace mqtt_internal

#endif // MQTT_MANAGER_H
