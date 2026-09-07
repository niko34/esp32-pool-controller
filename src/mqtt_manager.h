#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
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
  String filtrationStartState;    // feature-051 : heure début (HH:MM)
  String filtrationEndState;      // feature-051 : heure fin (HH:MM)
  String filtrationStartCommand;  // feature-051 : .../filtration_start/set
  String filtrationEndCommand;    // feature-051 : .../filtration_end/set
  String lightingState;        // État éclairage
  String lightingCommand;      // ON/OFF switch command
  String lightingScheduleState;    // feature-052 : programmation éclairage (ON/OFF)
  String lightingScheduleCommand;  // feature-052 : .../lighting_schedule/set
  String lightingStartState;       // feature-052 : heure début éclairage (HH:MM)
  String lightingEndState;         // feature-052 : heure fin éclairage (HH:MM)
  String lightingStartCommand;     // feature-052 : .../lighting_start/set
  String lightingEndCommand;       // feature-052 : .../lighting_end/set
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
  String phRegulationModeCommand;  // feature-009 : commande mode régulation pH
  String orpRegulationModeCommand; // feature-009 : commande mode régulation ORP
  String alertsTopic;
  String logsTopic;
  String statusTopic;          // LWT et status
  String diagnosticTopic;      // Diagnostic détaillé
  String alertsCalibrationTopic;  // feature-021 : alerte calibration EZO requise (retain)
  String alertsSensorStaleTopic;  // feature-021 : alerte capteur stale > kSensorStaleTimeoutMs
  String alertsSensorFrozenTopic; // feature-022 : alerte capteur pH/ORP figé (variance nulle, retain)
  String phSensorProblemState;    // feature-022 : problème capteur pH (stale OU figé, ON/OFF retain)
  String orpSensorProblemState;   // feature-022 : problème capteur ORP (stale OU figé, ON/OFF retain)
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
  // v2.20.3 (C3) : alarme « filtre non prêt durablement » (≥ 5 min avec eau et
  // mesures représentatives) = régulation À L'ARRÊT. Distinct de *_filter_ready,
  // qui passe légitimement à OFF à chaque warmup : ce topic-ci ne s'allume QUE
  // sur une panne. ON/OFF retain, device_class `problem` côté HA.
  String phFilterStuckState;
  String orpFilterStuckState;
  String phMixingDelayActiveState;  // ON/OFF pause mélange pH
  String orpMixingDelayActiveState; // ON/OFF pause mélange ORP
  // feature-050 : cumuls journaliers injectés + commandes volume quotidien + reboot
  String phDailyMlState;            // Cumul pH injecté aujourd'hui (mL, retain)
  String orpDailyMlState;           // Cumul ORP injecté aujourd'hui (mL, retain)
  String phDailyTargetMlCommand;    // Commande volume quotidien pH (mode programmée)
  String orpDailyTargetMlCommand;   // Commande volume quotidien ORP (mode programmée)
  String rebootCommand;             // Commande redémarrage (bouton HA)
  String boostState;                // feature-053 : état Mode Boost (ON/OFF, retain)
  String boostCommand;              // feature-053 : .../boost/set (ON/OFF)
  // feature-056 : mode d'installation + signal filtration externe
  String installModeState;          // Mode d'installation publié (managed/powered/external, retain)
  String installModeCommand;        // .../install_mode/set (managed/powered/external)
  String filtrationExternalStateCommand;  // .../filtration_external_state/set (ON/OFF, NON-retain) → setExternalState (signal filtration externe)
  // v2.26.0 (C5) : terme intégral pH — observabilité de la levée de Ki. Trois
  // topics retain, dédupliqués. Aucun équivalent ORP (Ki_ORP reste 0).
  String phIntegralState;           // Intégrale accumulée (pH·s, 1 décimale)
  String phIntegralTermState;       // Contribution au débit demandé (mL/min, 3 décimales)
  String phIntegralPctState;        // Remplissage de l'anti-windup (% de integralMax)
  // feature-085 (v2.28.0) : feedforward pH (dose de base apprise). Deux topics
  // scalaires retain, dédupliqués — même pattern que ph_integral* (il n'existe
  // pas de payload JSON d'état de régulation : une grandeur = un topic, et une
  // même grandeur porte le même nom sur les trois transports /data, WS, MQTT).
  String phFfFlowState;             // Débit de base appliqué (mL/min, 2 décimales, 0 hors session auto)
  String phFfStatusState;           // Code PhFfStatus (0 inactif, 1 actif, 2 latch, 3 historique<3j, 4 quota)
  // feature-092 (v2.46.0) : COMPOSITION du débit d'injection. Cinq topics
  // scalaires retain, dédupliqués, mêmes noms que les champs /data et WS.
  // DEUX champs de la carte sont volontairement HORS MQTT :
  //   - `ph_i_term_ml_per_min` : doublon visuel de l'entité `_ph_integral_term`
  //     déjà en production (même grandeur, servie brute) ;
  //   - `orp_sustainable_ml_per_min` : miroir de `ph_sustainable_ml_per_min`,
  //     qui n'est PAS publié non plus (vérifié) — on ne crée pas l'asymétrie.
  String phFfLearnedFlowState;      // Dose de base APPRISE (mL/min, 2 décimales, stable hors session)
  String phPTermFlowState;          // Écart courant kp·e (mL/min, 2 décimales, SIGNÉ)
  String phDemandFlowState;         // Demande totale bornée (mL/min, 2 décimales)
  String orpPTermFlowState;         // Écart courant ORP (mL/min, 2 décimales, SIGNÉ)
  String orpDemandFlowState;        // Demande ORP = terme P borné (mL/min, 2 décimales)
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
  // bug-race-refreshtopics-uaf : protège `topics` contre la course inter-tâches
  // (réassignation de String = free/realloc du buffer → UAF chez un lecteur).
  // CONTRAT : l'UNIQUE writer est refreshTopics() (mqttTask, + begin() avant la
  // création de la tâche) ; la SEULE lecture cross-task à protéger est la copie
  // du topic dans enqueueOutbound() (appelable depuis loopTask). Tous les autres
  // lecteurs de topics.* s'exécutent dans mqttTask (même tâche que le writer).
  // Mutex NON-récursif : ne jamais logger ni enqueuer un message en le tenant
  // (publishLog → enqueueOutbound reprendrait ce mutex → deadlock).
  SemaphoreHandle_t topicsMutex = nullptr;
  bool discoveryPublished = false;
  // feature-109 : numéro de rafale de découverte HA, MONOTONE — jamais remis à
  // zéro (cf. l'appel de publishDiscovery() dans onConnected). Il distingue au
  // journal deux reconnexions rapprochées dont la ligne de synthèse serait
  // sinon identique. Accédé UNIQUEMENT depuis mqttTask, comme discoveryPublished.
  uint32_t _discoveryBurstSeq = 0;

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

  // feature-050 : redémarrage différé demandé via MQTT/HA. Flag posé ET consommé
  // UNIQUEMENT depuis loopTask (drainCommandQueue) — même séquence propre que la
  // route POST /reboot : délai kRestartApModeDelayMs, puis shutdownForRestart()
  // (flush status=offline) et ESP.restart(). Jamais de restart direct au drain.
  bool _rebootPending = false;
  unsigned long _rebootRequestedAtMs = 0;

  void publishDiscovery();
  void refreshTopics();

  // feature-021 : caches pour publication edge-triggered de l'alerte calibration
  // et des états cal points. Lus/écrits uniquement depuis mqttTask.
  int  _lastPhCalPoints  = -2;  // -2 = jamais publié, -1..3 = valeurs réelles
  int  _lastOrpCalPoints = -2;
  bool _lastSensorStale  = false; // Cache pour alerte sensor_stale (NaN sur pH OU ORP)
  bool _lastSensorFrozen = false; // feature-022 : cache alerte sensor_frozen (pH OU ORP figé)

  // feature-022 : caches dédup des états binaires {base}/ph_sensor_problem et
  // {base}/orp_sensor_problem. -1 = jamais publié (force la 1ʳᵉ publication),
  // 0 = OFF, 1 = ON. Lus/écrits uniquement depuis mqttTask.
  int8_t _lastPhSensorProblem  = -1;
  int8_t _lastOrpSensorProblem = -1;

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
  // feature-050 : slots 14/15 réservés aux cumuls journaliers ph_daily_ml/orp_daily_ml.
  // v2.20.3 : +2 slots (16/17) pour ph/orp_filter_stuck.
  // v2.26.0 (C5) : +3 slots (18/19/20) pour ph_integral / _term / _pct — l'intégrale
  // est FIGÉE hors dosage (aucune erreur intégrée) : la déduplication supprime la
  // quasi-totalité du trafic de ces trois topics pendant les heures d'arrêt.
  // feature-085 : +2 slots (21/22) pour ph_ff_ml_per_min / ph_ff_status — figés
  // hors session automatique (0 / code stable) : la déduplication supprime tout
  // le trafic hors dosage, comme pour l'intégrale.
  static constexpr int kDedupCacheSlots = 28;  // feature-092 : 23 → 28 (5 topics de composition)
  String _lastFilterPub[kDedupCacheSlots];
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
  void publishBoostState();  // feature-053 : état Mode Boost (ON/OFF, retain)
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
  PhRegulationMode,  // payload = "automatic"|"scheduled"|"manual" (feature-009)
  OrpRegulationMode, // payload = "automatic"|"scheduled"|"manual" (feature-009)
  PhDailyTarget,     // payload = "150" (mL, entier — feature-050)
  OrpDailyTarget,    // payload = "500" (mL, entier — feature-050)
  Reboot,            // payload = "PRESS" (tout payload accepté — feature-050)
  FiltrationStart,   // payload = "08:00" (HH:MM — feature-051)
  FiltrationEnd,     // payload = "20:00" (HH:MM — feature-051)
  LightingSchedule,  // payload = "ON"|"OFF" (feature-052)
  LightingStart,     // payload = "20:00" (HH:MM — feature-052)
  LightingEnd,       // payload = "23:00" (HH:MM — feature-052)
  Boost,             // payload = "ON"|"OFF" (feature-053 : Mode Boost)
  FiltrationExternalState,  // payload = "ON"|"OFF" (feature-056 : signal filtration externe)
  InstallMode,       // payload = "managed"|"powered"|"external" (feature-056)
};

struct InboundCmd {
  InboundCmdType type;
  char payload[64];
};

}  // namespace mqtt_internal

#endif // MQTT_MANAGER_H
