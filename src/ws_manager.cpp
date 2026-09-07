#include <WiFi.h>
#include <time.h>
#include <esp_system.h>
#include <new>  // std::bad_alloc (filet TOCTOU autour de textAll, bug-heap-toctou A1)
#include "ws_manager.h"
#include "web_helpers.h"
#include "heap_metrics.h"  // heapLargestFreeBlock8Bit — sans log ni alloc (bug-080)
#include "config.h"
#include "constants.h"
#include "sensors.h"
#include "filtration.h"
#include "measure_validity.h"
#include "pump_controller.h"
#include "time_to_target.h"  // feature-096 : instantané servi en parité avec /data
#include "web_routes_control.h"
#include "mqtt_manager.h"
#include "lighting.h"
#include "auth.h"
#include "json_compat.h"

// Signal réseau secondaire (v2.9.2, défini dans main.cpp) : une connexion ou un
// message WS reçu prouve qu'un client web est actif → rafraîchit le watchdog de
// connectivité pour ne jamais rebooter pendant une utilisation active de l'appli.
void notifyNetworkActivity();

static const char* getResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWER_ON";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_EXT:       return "EXTERNAL";
    default:                return "UNKNOWN";
  }
}

WsManager wsManager;

// Buffer STATIQUE de construction des trames WS sortantes (bug-heap-toctou A2,
// pattern « réserver une fois, réutiliser toujours ») : alloué au link en .bss,
// hors heap, zéro fragmentation — supprime la String temporaire par push (churn
// G2 résiduel : ~2 Ko alloués/libérés toutes les 5 s).
// INVARIANT BLOQUANT : ce buffer est réservé à loopTask (les 3 broadcasts sont
// tous appelés depuis update()/loop aujourd'hui). Tout futur câblage MULTI-TÂCHE
// de broadcastLog (ex. callback logger depuis async_tcp/mqttTask) devra utiliser
// un buffer DÉDIÉ — jamais celui-ci, sous peine de trames entrelacées.
static char sWsFrameBuf[kWsFrameBufBytes];

// =============================================================================
// begin / update
// =============================================================================

void WsManager::begin(AsyncWebServer* server) {
  _ws = new AsyncWebSocket("/ws");
  _ws->onEvent([this](AsyncWebSocket* ws, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    _onEvent(ws, client, type, arg, data, len);
  });
  server->addHandler(_ws);

  systemLogger.info("WebSocket démarré sur /ws (push capteurs toutes les 5s)");
}

void WsManager::update() {
  if (!_ws) return;
  _ws->cleanupClients(4);  // Max 4 clients WS simultanés pour préserver les sockets lwIP

  // Récolte des clients gelés (churn-heap Q1) : déléguée au FORK, pas de boucle
  // manuelle ici. Revue 2.11.1 : itérer getClients() depuis loopTask est un UB —
  // _handleDisconnect (async_tcp) érase la liste sous le verrou PRIVÉ du fork,
  // inaccessible côté appelant. À la place : -DWS_MAX_QUEUED_MESSAGES=5
  // (platformio.ini) + setCloseClientOnQueueFull(true) à la connexion → le fork
  // ferme lui-même, sous SON verrou, tout client dont la file atteint 5 trames
  // (~25 s de snapshots non consommés = client gelé ; 32 par défaut = ~58 Ko
  // épinglés). L'état capteurs étant idempotent, aucune perte fonctionnelle.

  // Warn throttlé des pushes sautés par la garde heap (bug-badalloc-ws-broadcast).
  // Émis ICI (loopTask) et UNIQUEMENT ici — jamais dans _heapAllowsPush() (invariant
  // anti-récursion). Placé AVANT le return « aucun client » pour tracer aussi les
  // sauts survenus juste avant une déconnexion des clients.
  {
    uint32_t skipped = _skippedPushes.load(std::memory_order_relaxed);
    unsigned long now = millis();
    if (skipped > _lastWarnedSkipped &&
        (_lastSkipWarnMs == 0 || now - _lastSkipWarnMs >= kWsSkipWarnThrottleMs)) {
      // Réserve du contre-visa 02/09 (bug-080) : afficher la MÊME métrique que
      // celle qui décide du saut (8 bits) — la brute (~27 Ko d'IRAM inutilisable
      // en 8 bits) rendait ce diagnostic trompeur.
      systemLogger.warning(String(skipped - _lastWarnedSkipped) +
          " pushes WS sautés (heap fragmenté, maxBloc8=" +
          String(heapLargestFreeBlock8Bit()) + " o)");
      // Capturé AVANT le warn : si ce warn est lui-même sauté par broadcastLog
      // (futur callback logger), l'incrément sera compté au prochain warn.
      _lastWarnedSkipped = skipped;
      _lastSkipWarnMs = now;
    }
  }

  // Warn throttlé dédié des trames dépassant le buffer statique (bug-heap-toctou
  // A2) : push sauté, JAMAIS tronqué — signale qu'il faut agrandir kWsFrameBufBytes.
  // Même invariant anti-récursion : émis ICI (loopTask), jamais dans les broadcasts.
  {
    uint32_t oversized = _oversizedFrames.load(std::memory_order_relaxed);
    unsigned long now = millis();
    if (oversized > _lastWarnedOversized &&
        (_lastOversizedWarnMs == 0 || now - _lastOversizedWarnMs >= kWsSkipWarnThrottleMs)) {
      systemLogger.warning(String(oversized - _lastWarnedOversized) +
          " trame(s) WS > " + String((unsigned)kWsFrameBufBytes) +
          " o (buffer statique) — pushes sautés, agrandir kWsFrameBufBytes");
      _lastWarnedOversized = oversized;
      _lastOversizedWarnMs = now;
    }
  }

  if (_authenticatedClients.empty()) return;

  if (_pendingInitialPush) {
    _pendingInitialPush = false;
    _pendingConfigBroadcast = false;  // initial push couvre déjà la config
    broadcastSensorData();
    broadcastConfig();
    _lastSensorPush = millis();
    return;
  }

  if (_pendingConfigBroadcast) {
    _pendingConfigBroadcast = false;
    broadcastConfig();
  }

  if (millis() - _lastSensorPush >= kSensorPushIntervalMs) {
    broadcastSensorData();
    _lastSensorPush = millis();
  }
}

void WsManager::requestConfigBroadcast() {
  _pendingConfigBroadcast = true;
}

bool WsManager::hasClients() const {
  return _ws && _ws->count() > 0;
}

size_t WsManager::clientCount() const {
  return _ws ? _ws->count() : 0;
}

uint32_t WsManager::skippedPushCount() const {
  return _skippedPushes.load(std::memory_order_relaxed);
}

uint32_t WsManager::oversizedFrameCount() const {
  return _oversizedFrames.load(std::memory_order_relaxed);
}

// Garde heap commune des 3 broadcasts (bug-badalloc-ws-broadcast) : textAll() alloue
// un shared buffer (makeSharedBuffer, fork ESPAsyncWebServer) SANS rattraper
// std::bad_alloc → abort() (coredump 2026-07-28, payload 1777 o, maxBloc < ~1,8 Ko).
// Si le plus gros bloc contigu est sous kWsPushMinAllocBytes, on saute le push : l'UI
// rate un cycle de 5 s (sans coût UX) et le débounce auto-guérison se joue.
// bug-080 (v2.33.0) : mesure basculée sur heapLargestFreeBlock8Bit() — la métrique
// brute ESP.getMaxAllocHeap() comptait l'IRAM 32 bits (~27,5 Ko figés) et cette
// garde ne sautait donc JAMAIS un push, même en famine réelle. heapLargestFreeBlock8Bit
// est sans log ni allocation (contrat heap_metrics.h) : l'invariant ci-dessous tient.
// INVARIANT : ce helper ne logge JAMAIS — chaîne de ré-entrance sinon :
// logger → callback (futur) → broadcastLog → _heapAllowsPush → log → récursion.
// Le warn vit EXCLUSIVEMENT dans update() (throttlé kWsSkipWarnThrottleMs).
bool WsManager::_heapAllowsPush() {
  if (heapLargestFreeBlock8Bit() >= kWsPushMinAllocBytes) return true;
  _skippedPushes.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// =============================================================================
// Événements WebSocket
// =============================================================================

void WsManager::_onEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                          AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    notifyNetworkActivity();  // client actif : signal réseau secondaire
    _onClientConnect(client, reinterpret_cast<AsyncWebServerRequest*>(arg));
  } else if (type == WS_EVT_DATA) {
    notifyNetworkActivity();  // trafic WS entrant : signal réseau secondaire
    _onData(client, data, len);
  } else if (type == WS_EVT_DISCONNECT) {
    _authenticatedClients.erase(client->id());
  }
}

void WsManager::_onClientConnect(AsyncWebSocketClient* client, AsyncWebServerRequest* request) {
  // Anti-zombie (churn-heap Q1/Q2) : un client qui n'ACK plus retient ses buffers
  // de trames connexion ouverte (générateur n°1 du churn heap). Deux mécanismes
  // complémentaires, tous deux exécutés par le fork sous SON verrou :
  // - file d'envoi bornée à WS_MAX_QUEUED_MESSAGES=5 (platformio.ini) +
  //   setCloseClientOnQueueFull(true) → client actif-mais-gelé fermé à saturation ;
  // - PING auto toutes les kWsKeepAlivePeriodS s (fork : keepAlivePeriod(uint16_t
  //   secondes), ×1000 en interne). Nuance revue 2.11.1 : le fork n'envoie le PING
  //   que si la file du client est VIDE → ne récolte que les zombies inactifs
  //   (aucun push en attente) ; les zombies sous flux de pushes sont couverts par
  //   la saturation de file ci-dessus.
  client->setCloseClientOnQueueFull(true);
  client->keepAlivePeriod(kWsKeepAlivePeriodS);

  if (!authCfg.enabled) {
    // Pas d'auth : client immédiatement autorisé
    _authenticatedClients.insert(client->id());
    _pendingInitialPush = true;
  }
  // Si auth activée : attendre le message {"type":"auth","token":"..."} dans _onData
}

void WsManager::_onData(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
  StaticJson<256> doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;
  if (doc["type"] != "auth") return;

  String token = doc["token"] | "";
  // feature-028 : comparaison à temps constant (même exigence que l'auth HTTP)
  if (authCfg.enabled && !authManager.secureTokenEquals(token)) {
    systemLogger.warning("[WS] Token rejeté");
    client->close();
    return;
  }
  _authenticatedClients.insert(client->id());
  _pendingInitialPush = true;
}

// =============================================================================
// Broadcasts
// =============================================================================

void WsManager::broadcastSensorData() {
  if (!_ws || _ws->count() == 0) return;
  if (!_heapAllowsPush()) return;  // Garde AVANT la construction du JSON (bug-badalloc-ws-broadcast)
  size_t len = _buildSensorJson(sWsFrameBuf, sizeof(sWsFrameBuf));
  if (len == 0) {
    // Trame > buffer statique : push sauté, jamais tronqué (warn throttlé dans update()).
    _oversizedFrames.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Filet TOCTOU (bug-heap-toctou A1) : la garde heap peut être prise de vitesse
  // (async_tcp consomme le bloc mesuré entre le check et l'alloc). L'exception
  // std::bad_alloc part de makeSharedBuffer AVANT toute mutation de file dans le
  // fork → catch propre = push sauté, compté dans _skippedPushes (warn throttlé
  // existant d'update(), inchangé).
  try {
    _ws->textAll(sWsFrameBuf, len);
  } catch (const std::bad_alloc&) {
    _skippedPushes.fetch_add(1, std::memory_order_relaxed);
  }
}

void WsManager::broadcastConfig() {
  if (!_ws || _ws->count() == 0) return;
  if (!_heapAllowsPush()) return;  // Garde AVANT la construction du JSON (bug-badalloc-ws-broadcast)
  size_t len = _buildConfigJson(sWsFrameBuf, sizeof(sWsFrameBuf));
  if (len == 0) {
    _oversizedFrames.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Filet TOCTOU : voir broadcastSensorData (bad_alloc levé AVANT mutation de file).
  try {
    _ws->textAll(sWsFrameBuf, len);
  } catch (const std::bad_alloc&) {
    _skippedPushes.fetch_add(1, std::memory_order_relaxed);
  }
}

void WsManager::broadcastLog(const LogEntry& entry) {
  if (!_ws || _ws->count() == 0) return;
  if (!_heapAllowsPush()) return;  // Garde AVANT la construction du JSON (bug-badalloc-ws-broadcast)
  StaticJson<192> doc;
  doc["type"] = "log";
  JsonObject d = doc["data"].to<JsonObject>();
  d["timestamp"] = entry.timestamp;
  d["level"] = systemLogger.getLevelString(entry.level);
  d["message"] = entry.message;
  // Même buffer statique que les autres broadcasts (invariant loopTask — voir
  // sWsFrameBuf : un futur câblage multi-tâche devra utiliser un buffer dédié).
  if (measureJson(doc) + 1 > sizeof(sWsFrameBuf)) {
    _oversizedFrames.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  size_t len = serializeJson(doc, sWsFrameBuf, sizeof(sWsFrameBuf));
  // Filet TOCTOU : voir broadcastSensorData (bad_alloc levé AVANT mutation de file).
  try {
    _ws->textAll(sWsFrameBuf, len);
  } catch (const std::bad_alloc&) {
    _skippedPushes.fetch_add(1, std::memory_order_relaxed);
  }
}

// =============================================================================
// Construction JSON
// =============================================================================

size_t WsManager::_buildSensorJson(char* buf, size_t cap) const {
  // Buffer +64 octets vs version 1 sonde : champs temperature_circuit / sondes_identified / sondes_detected (feature-020)
  // feature-024 : +4 champs phSlope* (~80 octets) → bump à 1024.
  // feature-025 : +14 champs filtre pH/ORP + mixing/blocked (~300 octets) → bump à 1408.
  // feature-006 : +2 champs ph/orp_stab_remaining_s (~64 octets) → bump à 1472.
  // feature-011 : +2 champs ph/orp_scheduled_flow_ml_per_min (~80 octets) → bump à 1536.
  // feature-053 : +2 champs boost_active/boost_until (~48 octets) → bump à 1600.
  // feature-055 : +2 booléens boost_filtration_extended/boost_chlorine_boosted → bump à 1664.
  // feature-056 : +install_mode/water_present/filtration_state_source/_stale +
  //   filtration_ext_known/_on/_age_s (~200 octets) → bump à 1920.
  // v2.19.1 : +ph/orp_mix_remaining_s (~64 octets, observabilité pause mélange) → bump à 2048.
  // feature-061 : +measures_valid/measures_stab_remaining_s (~64 octets) → bump à 2112.
  // v2.20.3 (C2) : +2 booléens d'alarme filtre. DÉCOMPTE EXACT des octets
  // SÉRIALISÉS ajoutés (pire cas, valeur `false` — la plus longue) :
  //   "phFilterAlarm":false,   → 1+13+1+1+5+1 = 22 octets
  //   "orpFilterAlarm":false,  → 1+14+1+1+5+1 = 23 octets
  //   total = 45 octets. StaticJson dimensionne le POOL ArduinoJson (nœuds), pas
  //   la chaîne : 2 nœuds booléens supplémentaires ≈ 2 × 16 = 32 o de pool + les
  //   clés (littéraux, non copiés). Marge de 64 o retenue pour couvrir les deux
  //   (pool ET alignement) → bump 2112 → 2176. sWsFrameBuf est vérifié
  //   indépendamment par le garde `measureJson(doc) + 1 > sizeof(sWsFrameBuf)`.
  // Itération 3 (bug-dosage-fantome, défaut n°3) : +7 champs calendrier
  // d'impulsion. DÉCOMPTE des octets SÉRIALISÉS au pire cas :
  //   "pulse_period_ms":60000,  = 24 o | "ph_pulse_session":false, = 25 o
  //   "orp_pulse_session":false, = 26 o | "ph_pulse_ms":60000, = 20 o
  //   "orp_pulse_ms":60000, = 21 o | "ph_pulse_phase_ms":60000, = 26 o
  //   "orp_pulse_phase_ms":60000, = 27 o → total 169 o. Pool : 7 nœuds ≈ 112 o
  //   (clés littérales non copiées). Bump 2176 → 2432 (marge alignement incluse).
  // D2 (v2.25.0) : +2 booléens de butée BRUTE. Octets SÉRIALISÉS ajoutés :
  //   "phRawGuardBlocked":false,    → 1+17+1+1+5+1 = 26 o
  //   "orpPhRawGuardBlocked":false, → 1+20+1+1+5+1 = 29 o  → 55 o.
  // TAILLE RÉELLE MESURÉE de la trame (harnais hôte ArduinoJson 7.4.3, valeurs
  // PIRE CAS : causes de refus les plus longues, uint32 saturés, flottants à 9
  // chiffres) : 2373 o AVANT, 2428 o APRÈS. La trame tient donc largement dans
  // sWsFrameBuf (kWsFrameBufBytes = 3584 o, marge 1156 o) — INCHANGÉ, aucune
  // troncature possible (garde `measureJson(doc) + 1 > cap` en fin de fonction).
  // Le gabarit ci-dessous est porté 2432 → 2560 pour rester ≥ au pire cas mesuré.
  // v2.25.1 : +2 champs de CODE de cause de refus. Octets SÉRIALISÉS au pire cas
  // (valeur `null`, 4 caractères, plus longue que le plus grand code « 250 ») :
  //   "phDoseBlockedCode":null,  → 1+17+1+1+4+1 = 25 o
  //   "orpDoseBlockedCode":null, → 1+18+1+1+4+1 = 26 o   → 51 o.
  // Pire cas de trame : 2428 o (mesure v2.25.0) + 51 = 2479 o, soit encore
  // 1105 o de marge sous sWsFrameBuf (kWsFrameBufBytes = 3584 o) et sous le
  // gabarit documentaire 2560 — aucun bump nécessaire, aucune troncature
  // possible (garde `measureJson(doc) + 1 > cap` en fin de fonction, qui SAUTE
  // le push au lieu de tronquer).
  // v2.26.0 (C5) : +3 champs du terme intégral pH. Octets SÉRIALISÉS au pire cas
  // (flottants arrondis, signe compris) :
  //   "ph_integral":-43200.0,    → 1+11+1+1+8+1 = 23 o
  //   "ph_integral_term":-6.000, → 1+16+1+1+6+1 = 26 o
  //   "ph_integral_pct":100.0,   → 1+15+1+1+5+1 = 24 o   → 73 o.
  // v2.26.0 (revue, mineure n°1) : +1 booléen de VERDICT de saturation (parité /data)
  //   "ph_integral_saturated":false, → 1+21+1+1+5+1 = 30 o.
  // Pire cas de trame : 2479 o (mesure v2.25.1) + 73 + 30 = 2582 o. Le gabarit
  // documentaire est porté 2560 → 2688 pour rester franchement au-dessus du pire
  // cas ; la marge sous sWsFrameBuf (kWsFrameBufBytes = 3584 o) reste de 1002 o.
  // Aucune troncature possible (garde `measureJson(doc) + 1 > cap` en fin de
  // fonction, qui SAUTE le push au lieu de tronquer).
  // feature-085 (v2.28.0) : +2 champs feedforward pH (parité /data). Octets
  // SÉRIALISÉS au pire cas (flottant 2 décimales, code 1 chiffre) :
  //   "ph_ff_ml_per_min":123.45, → 1+16+1+1+6+1 = 26 o
  //   "ph_ff_status":4,          → 1+13+1+1+1+1 = 18 o   → 44 o.
  // Pire cas de trame : 2582 + 44 = 2626 o, sous le gabarit 2688 (marge 62 o) et
  // sous sWsFrameBuf (kWsFrameBufBytes = 3584 o, marge 958 o) — aucun bump.
  // feature-086 (v2.29.0) : +4 champs limite d'actionneur pH (parité /data).
  // Octets SÉRIALISÉS au pire cas (flottant 2 décimales, entier 7 chiffres,
  // booléens `false`) :
  //   "ph_sustainable_ml_per_min":123.45,  → 1+25+1+1+6+1 = 35 o
  //   "ph_daily_max_ml":1439986,           → 1+15+1+1+7+1 = 26 o
  //   "ph_limited_by_hourly_budget":false, → 1+27+1+1+5+1 = 36 o
  //   "ph_undersized":false,               → 1+13+1+1+5+1 = 22 o   → 119 o.
  // Pire cas de trame : 2626 + 119 = 2745 o, AU-DESSUS du gabarit 2688 → gabarit
  // porté 2688 → 2816. La marge sous sWsFrameBuf (kWsFrameBufBytes = 3584 o)
  // reste de 839 o — aucune troncature possible (garde `measureJson(doc) + 1 >
  // cap` en fin de fonction, qui SAUTE le push au lieu de tronquer).
  // feature-091 (v2.34.0) : purge adaptative — `measures_stab_remaining_s`
  // (39 o au pire cas) RETIRÉ, +4 champs (parité /data). Octets SÉRIALISÉS au
  // pire cas (uint32 saturés, uint8 à 3 chiffres) :
  //   "measures_stab_elapsed_s":4294967295,        → 1+23+1+1+10+1 = 37 o
  //   "measures_purge_last_duration_s":4294967295, → 1+30+1+1+10+1 = 44 o
  //   "measures_purge_last_end":4,                 → 1+23+1+1+1+1  = 28 o
  //   "measures_purge_count":255,                  → 1+20+1+1+3+1  = 27 o
  //   → +136 − 39 = +97 o. Pire cas de trame : 2745 + 97 = 2842 o, AU-DESSUS du
  // gabarit 2816 → gabarit porté 2816 → 2944. Marge sous sWsFrameBuf
  // (kWsFrameBufBytes = 3584 o) : 742 o — aucune troncature possible (garde
  // `measureJson(doc) + 1 > cap` en fin de fonction, qui SAUTE le push).
  // feature-096 (v2.37.0, 05/09/2026) : +8 champs « temps d'atteinte de cible »
  // (parité /data ↔ WS, MÊME passage). Octets SÉRIALISÉS au pire cas, comptés
  // clé par clé (guillemets + deux-points + valeur + virgule) :
  //   "ph_ttt_state":3,     → 1+12+1+1+1+1 = 17 o   (état 0-3, 1 chiffre)
  //   "ph_ttt_min":1440,    → 1+10+1+1+4+1 = 18 o   (borné à 1440 par le module pur)
  //   "ph_ttt_e0":-14.00,   → 1+ 9+1+1+6+1 = 19 o   (pH, 2 décimales signées ; `null` = 4 o, plus court)
  //   "ph_ttt_flag":255,    → 1+11+1+1+3+1 = 18 o   (CHAMP DE BITS uint8, jusqu'à 3 chiffres)
  //   "orp_ttt_state":3,    → 1+13+1+1+1+1 = 18 o
  //   "orp_ttt_min":1440,   → 1+11+1+1+4+1 = 19 o
  //   "orp_ttt_e0":-1000.0, → 1+10+1+1+7+1 = 21 o   (mV, 1 décimale signée)
  //   "orp_ttt_flag":255,   → 1+12+1+1+3+1 = 20 o
  //   → 150 o (décompte IDENTIQUE à celui de web_routes_data.cpp — mêmes clés).
  // Pire cas de trame : 2842 (v2.34.0) + 150 = 2992 o, AU-DESSUS du gabarit
  // documentaire 2944 → gabarit porté 2944 → 3072 (marge 80 o).
  // feature-092 (v2.46.0, 07/09/2026) : +8 champs de COMPOSITION du débit
  // d'injection (parité /data ↔ WS, MÊME passage, marqueurs PARITE-DATA) et,
  // côté /data seulement, +4 champs de PAUSE MÉLANGE qui étaient WS-seuls.
  //
  // MÉTHODE DE CHIFFRAGE — À LIRE AVANT DE TOUCHER À CE NOMBRE. On compte la
  // valeur au PIRE CAS STRUCTUREL, c'est-à-dire la borne du tampon ou du type
  // qui la porte, JAMAIS une valeur relevée sur une trame vivante. C'est
  // exactement l'erreur qui a rendu le chiffre précédent faux de 115 o : les
  // deux `*DoseBlockedReason` avaient été comptés ~62/64 o « au pire » alors
  // qu'ils sont bornés par kDoseBlockedReasonMaxLen = 96, soit 95 caractères
  // + 2 guillemets = 97 o de VALEUR chacun (contre ~8 o en fonctionnement
  // normal). Le décompte corrigé de ces deux clés :
  //   "phDoseBlockedReason":"<95 car.>",  → 1+19+1+1+97+1 = 120 o (comptée 62)
  //   "orpDoseBlockedReason":"<95 car.>", → 1+20+1+1+97+1 = 121 o (comptée 64)
  //   → correction +115 o sur le pire cas de la trame, sans qu'aucun champ soit
  //     ajouté. Ce pire cas n'est PAS théorique : les causes de refus les plus
  //     longues sont celles de l'ESCALADE DE SÉCURITÉ pH — le moment précis où
  //     perdre la trame coûte le plus cher.
  // Rappel du mode de défaillance côté WS : le dépassement SAUTE le push (garde
  // `measureJson(doc) + 1 > cap`), il ne TRONQUE jamais.
  //
  // Décompte des 8 nouvelles clés, une par une (1 guillemet + nom + guillemet +
  // deux-points + valeur + virgule). Valeurs signées comptées sur 8 caractères
  // (« -1234.56 ») : les termes p et i ne sont PAS bornés, c'est leur raison
  // d'être (grandeurs pédagogiques signées) ; les grandeurs ≥ 0 sur 6
  // (« 123.45 ») comme les champs mL/min existants ; `null` = 4 o, plus court,
  // donc jamais dimensionnant.
  //   "ph_ff_learned_ml_per_min":123.45,   → 1+24+1+1+6+1 = 34 o
  //   "ph_p_term_ml_per_min":-1234.56,     → 1+20+1+1+8+1 = 32 o
  //   "ph_i_term_ml_per_min":-1234.56,     → 1+20+1+1+8+1 = 32 o
  //   "ph_demand_ml_per_min":123.45,       → 1+20+1+1+6+1 = 30 o
  //   "orp_p_term_ml_per_min":-1234.56,    → 1+21+1+1+8+1 = 33 o
  //   "orp_demand_ml_per_min":123.45,      → 1+21+1+1+6+1 = 31 o
  //   "orp_sustainable_ml_per_min":123.45, → 1+26+1+1+6+1 = 36 o
  //   "orp_limited_by_hourly_budget":false, → 1+28+1+1+5+1 = 37 o
  //   → 265 o.
  // Décompte des 4 clés de pause mélange (uint32 comptés sur 10 chiffres — borne
  // du TYPE, pas la borne fonctionnelle de 900 s) :
  //   "phMixingDelayActive":false,      → 1+19+1+1+ 5+1 = 28 o
  //   "orpMixingDelayActive":false,     → 1+20+1+1+ 5+1 = 29 o
  //   "ph_mix_remaining_s":4294967295,  → 1+18+1+1+10+1 = 32 o
  //   "orp_mix_remaining_s":4294967295, → 1+19+1+1+10+1 = 33 o
  //   → 122 o (déjà comptés côté WS depuis v2.19.1 : ils y existaient déjà).
  // Correctif de parité du même cycle : +2 booléens de CARACTÉRISATION de pompe
  // (`ph/orp_pump_flow_uncharacterized`), jusqu'ici /data-SEULS — le push WS
  // effaçait le pied de carte qu'ils pilotent (piège n° 1 en sens inverse) :
  //   "ph_pump_flow_uncharacterized":false,  → 1+28+1+1+5+1 = 37 o
  //   "orp_pump_flow_uncharacterized":false, → 1+29+1+1+5+1 = 38 o   → 75 o.
  //
  // Pire cas de trame : 2992 (décompte v2.37.0) + 115 (correction ci-dessus)
  // + 265 (8 champs de composition) + 75 (2 booléens de parité) = 3447 o,
  // AU-DESSUS du gabarit 3072 → gabarit porté 3072 → 3456 (marge 8 o sous le
  // gabarit documentaire, qui n'est qu'un `capacity()` : aucun effet mémoire).
  //
  // ⚠️ MARGE SOUS LE SEUL PLAFOND DUR, kWsFrameBufBytes = 3584 o :
  // 3584 − 3447 = 137 o. C'EST SOUS LA BARRE DES 150 o, et c'est dit ici plutôt
  // qu'absorbé : le décompte v2.37.0 annonçait 592 o dont 115 n'ont jamais
  // existé (les deux `*DoseBlockedReason` comptés sur une trame vivante). 137 o,
  // c'est QUATRE clés flottantes. Tout nouveau champ de cette trame exige
  // désormais un retrait compensatoire OU un relèvement de kWsFrameBufBytes
  // (décision d'ADR, pas de routine).
  //
  // ÉLAGAGE CHIFFRÉ, NON APPLIQUÉ (arbitrage hors de ce fichier) : `ph_integral`
  // (pH·s) et `ph_integral_term` n'ont plus AUCUN consommateur côté client après
  // feature-092. Les retirer de CETTE TRAME seulement (en les gardant sur /data,
  // sur MQTT et donc dans Home Assistant, où ils sont un contrat externe)
  // rendrait :
  //   "ph_integral":-43200.0,    = 23 o
  //   "ph_integral_term":-6.000, = 26 o   → 49 o, portant la marge à 186 o.
  // Ce n'est PAS gratuit : les deux clés deviendraient /data-seules, ce qui
  // exige une exception motivée dans la sentinelle de parité. Ne rien retirer
  // sans cette décision.
  // MARGE RÉSIDUELLE SOUS LE SEUL PLAFOND DUR, sWsFrameBuf
  // (kWsFrameBufBytes = 3584 o, constants.h) : 3584 − 2992 = 592 o (elle était
  // de 742 o avant cet ajout). Aucun bump de kWsFrameBufBytes n'est requis.
  // Ce que fait la garde `measureJson(doc) + 1 > cap` en fin de fonction si un
  // jour la trame dépassait : elle SAUTE le push et incrémente _oversizedFrames
  // (warn throttlé, plus haut dans ce fichier) — elle ne TRONQUE jamais.
  // NB : en ArduinoJson 7, StaticJson<N> dérive de JsonDocument et N n'est plus
  // qu'une valeur documentaire (capacity()) — le pool est alloué dynamiquement.
  // Le seul plafond DUR de ce chemin est kWsFrameBufBytes.
  //
  // bug-082 (v2.47.1) : +1 objet `flow_assist` de DEUX champs (`aborted`,
  // `flow255[2]`), correctif de parité — cf. le bloc commenté plus bas dans
  // cette fonction pour le POURQUOI (garde-fou de dosage, pas un affichage).
  //
  // MÉTHODE DE CHIFFRAGE APPLIQUÉE (ADR-0073) — À RELIRE AVANT DE TOUCHER À CE
  // NOMBRE, ET À REFAIRE PLUTÔT QU'À RECOPIER. On compte la BORNE STRUCTURELLE
  // du type qui porte la valeur, jamais une valeur relevée sur une trame
  // vivante : c'est cette confusion qui a rendu QUATRE chiffrages successifs de
  // ce budget faux. Le chiffrage annoncé pour bug-082 (66 o, « flottant
  // ArduinoJson à 9 chiffres significatifs ») était lui-même faux — recompté
  // ci-dessous à 70 o.
  //
  // Deux bornes, toutes deux VÉRIFIÉES et non déduites :
  //   – un élément de `flow255` est un `float` sérialisé par ArduinoJson 7.
  //     Balayage EXHAUSTIF des 2^31 bit-patterns finis ≥ 0 (harnais hôte,
  //     ArduinoJson des libdeps du projet) : longueur maximale = 12 o, atteinte
  //     par la notation exponentielle (« 1.401298e-45 »). Ce n'est PAS
  //     9 chiffres significatifs + point (10 o) : ArduinoJson rend 7 chiffres
  //     significatifs, mais l'exposant reprend les 4 o économisés. Le résultat
  //     du même balayage restreint à la borne fonctionnelle du débit
  //     (0 < f ≤ 200, celle des bornes de saisie) est IDENTIQUE : 12 o.
  //   – un booléen vaut `false` au pire (5 o), plus long que `true` (4 o).
  // Décompte, structure comprise (chaque ligne : guillemet + nom + guillemet +
  // deux-points + valeur, et la virgule qui SUIT la ligne comptée avec elle —
  // `aborted` est le premier membre, la virgule de séparation est donc portée
  // par lui, et celle qui suit `}` sépare l'objet de la clé suivante) :
  //   "flow_assist":{                          → 1+11+1+1+1              = 15 o
  //     "aborted":false,                       → 1+ 7+1+1+5+1            = 16 o
  //     "flow255":[1.401298e-45,1.401298e-45]  → 1+ 7+1+1+1+12+1+12+1    = 37 o
  //   },                                       → 1+1                     =  2 o
  //   → 70 o au PIRE CAS structurel du TYPE.
  // Contre-mesure de vérification (même harnais, bloc inséré entre deux clés
  // factices pour capter la virgule) : 70 o au pire cas, 50 o en fonctionnement
  // (« 74 » = 2 o par élément). Les 50 o sont d'ailleurs la valeur EXACTE et non
  // une estimation : `kPumpFlow255MlPerMin` est une `constexpr float`, donc sa
  // sérialisation est figée à la compilation. On budgète quand même les 70 o :
  // ADR-0073 impose la borne du type, et cette constante est un paramètre de
  // caractérisation de pompe qu'un futur relevé au banc peut changer.
  //
  // Pire cas de trame : 3447 (décompte v2.40.0 corrigé) + 70 = 3517 o,
  // AU-DESSUS du gabarit 3456 → gabarit porté 3456 → 3584.
  //
  // MARGE RÉSIDUELLE SOUS LE SEUL PLAFOND DUR, sWsFrameBuf
  // (kWsFrameBufBytes = 3584 o, constants.h) : 3584 - 3517 = 67 o (elle était
  // de 137 o avant cet ajout). Rappel du mode de défaillance : le dépassement
  // SAUTE le push (garde `measureJson(doc) + 1 > cap` en fin de fonction, qui
  // incrémente _oversizedFrames et journalise un warn throttlé) — il ne TRONQUE
  // jamais. Et le pire cas de longueur COÏNCIDE avec l'ESCALADE DE SÉCURITÉ pH :
  // ce sont ses causes de refus qui saturent kDoseBlockedReasonMaxLen = 96, donc
  // le moment précis où perdre la trame coûte le plus cher. 67 o, c'est DEUX
  // clés flottantes. Tout nouveau champ de cette trame exige désormais un
  // retrait compensatoire OU un relèvement de kWsFrameBufBytes (décision d'ADR,
  // pas de routine).
  //
  // RÉSERVE DOCUMENTÉE, NON DÉPENSÉE ICI : retirer `ph_integral` (23 o) et
  // `ph_integral_term` (26 o) de CETTE SEULE trame — sans consommateur client
  // depuis feature-092 — rendrait 49 o et porterait la marge à 116 o. Cet
  // arbitrage exige une exception motivée dans la sentinelle de parité (les deux
  // clés deviendraient /data-seules) : il reste à prendre, il n'est PAS pris.
  StaticJson<3584> doc;
  doc["type"] = "sensor_data";
  JsonObject d = doc["data"].to<JsonObject>();

  // feature-021 : pH publié avec 3 décimales (l'EZO rend 3 décimales fiables, cf. spec ligne 247).
  // ORP reste en entier (mV) — la résolution physique du capteur ne justifie pas de décimales.
  // feature-025 : la "valeur courante" UI/MQTT (champs ph/orp) reflète désormais la valeur
  // FILTRÉE (getPhFiltered/getOrpFiltered), pas le brut. getPh()/getOrp() firmware restent
  // bruts (rétrocompat scheduled/diagnostic), mais l'utilisateur affiche la mesure lissée.
  // Le brut reste exposé séparément via phRaw/orpRaw pour diagnostic EMI. Si le filtre n'est
  // pas encore amorcé (NaN filtré), on retombe sur le brut pour ne pas afficher "--" au boot.
  // Lectures cachées en variables locales pour éviter une race entre le check `isnan` et le `round`.
  // feature-067 : côté pH, phRaw est le BRUT EZO (sans offset) — il n'alimente QUE
  // le champ de diagnostic `phRaw`. Le champ `ph` porte toujours une valeur CORRIGÉE
  // (filtrée-corrigée, ou getPh() au warmup) : on ne mélange jamais brut et corrigé
  // sur une même grandeur affichée.
  float orpRaw = sensors.getOrpRaw();
  float phRaw  = sensors.getPhRaw();          // BRUT EZO (diagnostic)
  float orpFiltered = sensors.getOrpFiltered();
  float phFiltered  = sensors.getPhFiltered();  // lissé CORRIGÉ
  float orpMedian = sensors.getOrpMedian();
  float phMedian  = sensors.getPhMedian();      // médiane CORRIGÉE
  // Valeur principale = filtrée si disponible, sinon dernière mesure (warmup), sinon null.
  float orpVal = !isnan(orpFiltered) ? orpFiltered : orpRaw;
  float phVal  = !isnan(phFiltered)  ? phFiltered  : sensors.getPh();  // corrigé dans les 2 cas
  float tVal   = sensors.getTemperature();
  // T° eau brute (sans offset utilisateur) : exposée pour permettre à l'UI de calibration
  // de calculer un nouvel offset à partir d'une référence externe sans dépendre de la
  // formule firmware. NaN si sonde "eau" non identifiée.
  float tRawWater = sensors.getWaterTemperatureRaw();
  if (!isnan(orpVal))     d["orp"] = orpVal;                                    else d["orp"] = nullptr;
  if (!isnan(phVal))      d["ph"]  = round(phVal * 1000.0f) / 1000.0f;          else d["ph"] = nullptr;
  // feature-025 : champs filtre — null si NaN/indisponible (EZO débranché → UI sans crash).
  if (!isnan(phRaw))      d["phRaw"] = round(phRaw * 1000.0f) / 1000.0f;        else d["phRaw"] = nullptr;
  if (!isnan(phMedian))   d["phMedian"] = round(phMedian * 1000.0f) / 1000.0f;  else d["phMedian"] = nullptr;
  if (!isnan(phFiltered)) d["phFiltered"] = round(phFiltered * 1000.0f) / 1000.0f; else d["phFiltered"] = nullptr;
  d["phFilterReady"]    = sensors.isPhFilterReady();
  d["phFilterUnstable"] = sensors.isPhFilterUnstable();
  d["phRejectedCount"]  = sensors.getPhRejectedCount();
  // v2.20.3 (C2) : alarme « filtre non prêt durablement » — cf. bump du buffer
  // en tête de fonction pour le décompte exact des octets ajoutés.
  d["phFilterAlarm"]    = sensors.isPhFilterNotReadyAlarm();
  if (!isnan(orpRaw))      d["orpRaw"] = round(orpRaw);                          else d["orpRaw"] = nullptr;
  if (!isnan(orpMedian))   d["orpMedian"] = round(orpMedian);                    else d["orpMedian"] = nullptr;
  if (!isnan(orpFiltered)) d["orpFiltered"] = round(orpFiltered);               else d["orpFiltered"] = nullptr;
  d["orpFilterReady"]    = sensors.isOrpFilterReady();
  d["orpFilterUnstable"] = sensors.isOrpFilterUnstable();
  d["orpRejectedCount"]  = sensors.getOrpRejectedCount();
  d["orpFilterAlarm"]    = sensors.isOrpFilterNotReadyAlarm();  // v2.20.3 (C2)
  // Pause mélange hydraulique active (post-injection) + secondes restantes :
  // feature-092 — ces quatre champs ont MIGRÉ dans le bloc `PARITE-DATA` en fin
  // de fonction. Ils étaient WS-SEULS (absents de /data), c'est-à-dire dans
  // l'état exact qui fait CLIGNOTER un affichage. Les regrouper avec leur
  // homologue /data, entre marqueurs, est ce qui rend la parité vérifiable.
  uint32_t nowMs = millis();
  // R2 (revue v2.25.0) : copie BORNÉE dans un tampon de pile (mêmes accesseurs
  // que /data — parité de transport). Ce chemin tourne en loopTask, mais garder
  // les deux côtés identiques évite qu'une future migration du WS vers un autre
  // contexte réintroduise la lecture de `String` inter-tâches. ArduinoJson
  // duplique un `char[]` non const : le tampon peut mourir après la trame.
  char phBlocked[kDoseBlockedReasonMaxLen];
  char orpBlocked[kDoseBlockedReasonMaxLen];
  PumpController.copyPhDoseBlockedReason(phBlocked, sizeof(phBlocked));
  PumpController.copyOrpDoseBlockedReason(orpBlocked, sizeof(orpBlocked));
  if (phBlocked[0]  != '\0') d["phDoseBlockedReason"]  = phBlocked;  else d["phDoseBlockedReason"]  = nullptr;
  if (orpBlocked[0] != '\0') d["orpDoseBlockedReason"] = orpBlocked; else d["orpDoseBlockedReason"] = nullptr;
  // v2.25.1 : CODE de cause de refus (identifiant STABLE) publié à côté du texte.
  // Le texte restait la seule source d'identification côté client, qui y cherchait
  // des fragments de phrase — d'où des causes fausses (warmup lu comme « EZO
  // indisponible »). Règles null IDENTIQUES à /data (web_routes_data.cpp), champs
  // ajoutés dans le MÊME passage : 0 = aucun refus → null explicite (absent ≠ 0).
  const uint8_t phDoseCode  = PumpController.getPhDoseBlockedCode();
  const uint8_t orpDoseCode = PumpController.getOrpDoseBlockedCode();
  if (phDoseCode  != kDoseBlockedCodeNone) d["phDoseBlockedCode"]  = phDoseCode;  else d["phDoseBlockedCode"]  = nullptr;
  if (orpDoseCode != kDoseBlockedCodeNone) d["orpDoseBlockedCode"] = orpDoseCode; else d["orpDoseBlockedCode"] = nullptr;
  // D2 (v2.25.0) : « la butée BRUTE mord » — état qu'AUCUN client ne peut
  // déduire du pH corrigé publié (un offset saisi à tort rend le corrigé
  // rassurant pendant que la mesure capteur est hors limite). Booléens STRICTS,
  // recalculés à chaque tick par la surveillance, sans latch (un latch
  // reproduirait le défaut D1). Noms et sémantique IDENTIQUES à /data
  // (web_routes_data.cpp) — parité de transport exigée.
  d["phRawGuardBlocked"]    = PumpController.isPhRawGuardBlocked();
  d["orpPhRawGuardBlocked"] = PumpController.isOrpPhRawGuardBlocked();
  if (!isnan(tVal))       d["temperature"] = tVal;                              else d["temperature"] = nullptr;
  if (!isnan(tRawWater))  d["temperature_raw"] = round(tRawWater * 100.0f) / 100.0f; else d["temperature_raw"] = nullptr;
  // feature-020 : 2ᵉ sonde DS18B20 "circuit" + indicateurs identification
  float tc = sensors.getCircuitTemperature();
  if (!isnan(tc))                          d["temperature_circuit"] = round(tc * 10.0f) / 10.0f; else d["temperature_circuit"] = nullptr;
  d["sondes_identified"] = sensors.areSondesIdentified();
  d["sondes_detected"]   = sensors.getDetectedSondeCount();

  // feature-021 : statut calibration EZO (lecture cache, pas d'I²C dans le chemin WS).
  d["phCalPoints"]  = sensors.getPhCalibrationPointsCached();
  d["orpCalPoints"] = sensors.getOrpCalibrationPointsCached();

  // feature-024 : pente sonde pH (cache lu sans I²C).
  // Arrondis : pentes à 1 décimale (résolution EZO), zéro à 2 décimales (mV).
  // null si jamais lu OU bus dégradé (NaN), l'UI affiche alors "—".
  float slopeAcid = sensors.getPhSlopeAcid();
  float slopeBase = sensors.getPhSlopeBase();
  float slopeZero = sensors.getPhSlopeZero();
  if (!isnan(slopeAcid))  d["phSlopeAcid"] = round(slopeAcid * 10.0f) / 10.0f;     else d["phSlopeAcid"] = nullptr;
  if (!isnan(slopeBase))  d["phSlopeBase"] = round(slopeBase * 10.0f) / 10.0f;     else d["phSlopeBase"] = nullptr;
  if (!isnan(slopeZero))  d["phSlopeZero"] = round(slopeZero * 100.0f) / 100.0f;   else d["phSlopeZero"] = nullptr;
  // phSlopeAgeMs : null si jamais lu (cohérent avec phSlope* nullables), sinon ms écoulés.
  uint32_t slopeAge = sensors.getPhSlopeAgeMs();
  if (slopeAge == UINT32_MAX) d["phSlopeAgeMs"] = nullptr;
  else                        d["phSlopeAgeMs"] = slopeAge;

  d["filtration_running"]  = filtration.isRunning();
  d["filtration_force_on"] = filtrationCfg.forceOn;
  d["filtration_force_off"] = filtrationCfg.forceOff;
  d["ph_dosing"]           = PumpController.isPhDosing();
  d["orp_dosing"]         = PumpController.isOrpDosing();
  // Itération 3 (bug-dosage-fantome, défaut n°3) : calendrier d'impulsion —
  // « session active » (ph/orp_dosing = commandé) vs « pompe alimentée » (pouls
  // effectif : phase < pulse_ms). Getters lecture seule ; phase RELATIVE au début
  // de période (le client l'ancre sur SA propre horloge à la réception). Noms de
  // champs IDENTIQUES à /data (web_routes_data.cpp) — contrat architect.
  // `nowMs` : variable locale déclarée plus haut dans cette fonction.
  d["pulse_period_ms"]    = kPulsePeriodMs;
  d["ph_pulse_session"]   = PumpController.isPulseSessionActive(0);
  d["orp_pulse_session"]  = PumpController.isPulseSessionActive(1);
  d["ph_pulse_ms"]        = PumpController.getPulseMs(0);
  d["orp_pulse_ms"]       = PumpController.getPulseMs(1);
  d["ph_pulse_phase_ms"]  = PumpController.getPulsePhaseMs(0, nowMs);
  d["orp_pulse_phase_ms"] = PumpController.getPulsePhaseMs(1, nowMs);
  d["ph_used_ms"]         = PumpController.getPhUsedMs();
  d["orp_used_ms"]        = PumpController.getOrpUsedMs();
  d["stabilization_remaining_s"] = PumpController.getStabilizationRemainingS();
  // feature-006 : stabilisation PAR POMPE LOGIQUE (0=pH, 1=ORP), miroir exact
  // de la garde manuelle firmware (manualInjectGuardOrReject). Le champ global
  // ci-dessus (max des 2) est conservé pour compat (badge global, ancien front).
  d["ph_stab_remaining_s"]  = PumpController.getStabilizationRemainingS(0);
  d["orp_stab_remaining_s"] = PumpController.getStabilizationRemainingS(1);
  // feature-011 : débit moyen planifié en mode "Programmée" (mL/min sur la plage de
  // filtration restante). NAN firmware = hors mode scheduled / hors plage / heure
  // invalide → null explicite côté WS (l'UI affiche "—").
  float phSchedFlow  = PumpController.getPhScheduledPlannedFlow();
  float orpSchedFlow = PumpController.getOrpScheduledPlannedFlow();
  if (!isnan(phSchedFlow))  d["ph_scheduled_flow_ml_per_min"]  = round(phSchedFlow * 10.0f) / 10.0f;  else d["ph_scheduled_flow_ml_per_min"]  = nullptr;
  if (!isnan(orpSchedFlow)) d["orp_scheduled_flow_ml_per_min"] = round(orpSchedFlow * 10.0f) / 10.0f; else d["orp_scheduled_flow_ml_per_min"] = nullptr;
  // v2.26.0 (C5) : TERME INTÉGRAL pH — observabilité de la levée de Ki (le terme
  // intégral fournit TOUTE la charge à l'équilibre, où le proportionnel s'annule ;
  // sans ces champs, une intégrale saturée resterait invisible).
  //   ph_integral      = intégrale accumulée (pH·s)
  //   ph_integral_term = contribution au débit moyen demandé (mL/min)
  //   ph_integral_pct  = remplissage de l'anti-windup (% de integralMax)
  //   ph_integral_saturated = VERDICT d'épisode de saturation (booléen) — v2.26.0
  //     (revue, mineure n°1). Le seuil (95 % de `demande / débit soutenable`) et
  //     son hystérésis restent au FIRMWARE : comparer soi-même 95 % au
  //     REMPLISSAGE (`ph_integral_pct`) confond deux grandeurs et annonce
  //     « vérifiez le bidon » sur une régulation saine.
  // Champs et règles STRICTEMENT identiques à /data (web_routes_data.cpp) : mêmes
  // noms, mêmes arrondis, NaN → null explicite. Pas d'équivalent ORP : Ki_ORP
  // reste 0 (asymétrie voulue, hors périmètre du gate v2.26.0).
  float phIntegral    = PumpController.getPhIntegral();
  float phIntegralTrm = PumpController.getPhIntegralTerm();
  float phIntegralPct = PumpController.getPhIntegralPct();
  if (!isnan(phIntegral))    d["ph_integral"]      = round(phIntegral * 10.0f) / 10.0f;         else d["ph_integral"]      = nullptr;
  if (!isnan(phIntegralTrm)) d["ph_integral_term"] = round(phIntegralTrm * 1000.0f) / 1000.0f;  else d["ph_integral_term"] = nullptr;
  if (!isnan(phIntegralPct)) d["ph_integral_pct"]  = round(phIntegralPct * 10.0f) / 10.0f;      else d["ph_integral_pct"]  = nullptr;
  d["ph_integral_saturated"] = PumpController.isPhIntegralSaturated();
  // feature-085 (v2.28.0) : FEEDFORWARD pH — dose de base apprise. Champs et
  // règles STRICTEMENT identiques à /data (web_routes_data.cpp) : MÊMES clés,
  // MÊME arrondi 2 décimales, NaN → null explicite (parité stricte — un champ
  // servi d'un seul côté CLIGNOTE dans l'UI, 5 récidives du motif).
  //   ph_ff_ml_per_min = débit de base appliqué à la session auto (mL/min, 0 hors session)
  //   ph_ff_status     = code PhFfStatus (0 inactif, 1 actif, 2 latch sécurité,
  //                      3 historique < 3 j, 4 quota ≥ 60 %) — contrat FIGÉ.
  float phFfFlow = PumpController.getPhFeedforwardFlow();
  if (!isnan(phFfFlow)) d["ph_ff_ml_per_min"] = round(phFfFlow * 100.0f) / 100.0f; else d["ph_ff_ml_per_min"] = nullptr;
  d["ph_ff_status"] = PumpController.getPhFeedforwardStatus();
  // feature-086 (v2.29.0) : LIMITE D'ACTIONNEUR pH — capacités et verdicts de
  // diagnostic. Champs et règles STRICTEMENT identiques à /data
  // (web_routes_data.cpp) : MÊMES clés, MÊMES arrondis, NaN → null explicite
  // (parité stricte — un champ servi d'un seul côté CLIGNOTE dans l'UI,
  // 5 récidives du motif). Accesseurs LECTURE SEULE (cf. pump_controller.cpp).
  //   ph_sustainable_ml_per_min   = plafond de débit moyen imposé par la limite
  //       horaire (mL/min, 2 décimales ; null si pompe non caractérisée)
  //   ph_daily_max_ml             = maximum injectable sur la journée de
  //       filtration PROGRAMMÉE (mL/j, entier ; null si non calculable)
  //   ph_limited_by_hourly_budget = épisode C5 ouvert ET mesure pH valide
  //   ph_undersized               = série ≥ 3 journées saturées ET anomalie ouverte
  const float phSust     = PumpController.getPhSustainableMlPerMin();
  const float phDailyMax = PumpController.getPhDailyMaxMl();
  if (!isnan(phSust))     d["ph_sustainable_ml_per_min"] = round(phSust * 100.0f) / 100.0f; else d["ph_sustainable_ml_per_min"] = nullptr;
  if (!isnan(phDailyMax)) d["ph_daily_max_ml"] = (uint32_t)lroundf(phDailyMax);             else d["ph_daily_max_ml"] = nullptr;
  d["ph_limited_by_hourly_budget"] = PumpController.isPhLimitedByHourlyBudget();
  d["ph_undersized"]               = PumpController.isPhUndersizedSuspected();
  d["ph_daily_ml"]        = safetyLimits.dailyPhInjectedMl;
  d["orp_daily_ml"]       = safetyLimits.dailyOrpInjectedMl;
  d["ph_limit_reached"]   = safetyLimits.phLimitReached;
  d["orp_limit_reached"]  = safetyLimits.orpLimitReached;

  // Volumes produits
  d["ph_tracking_enabled"]  = productCfg.phTrackingEnabled;
  d["ph_remaining_ml"]      = max(0.0f, productCfg.phContainerVolumeMl - productCfg.phTotalInjectedMl);
  d["ph_container_ml"]      = productCfg.phContainerVolumeMl;
  d["ph_alert_threshold_ml"]= productCfg.phAlertThresholdMl;
  d["orp_tracking_enabled"] = productCfg.orpTrackingEnabled;
  d["orp_remaining_ml"]     = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  d["orp_container_ml"]     = productCfg.orpContainerVolumeMl;
  d["orp_alert_threshold_ml"]= productCfg.orpAlertThresholdMl;

  d["ph_inject_remaining_s"]  = manualInjectRemainingS(manualInjectPh);
  d["orp_inject_remaining_s"] = manualInjectRemainingS(manualInjectOrp);

  // bug-082 (v2.47.1) — 5ᵉ RÉCIDIVE DU MOTIF DE PARITÉ. `flow_assist` n'existait
  // que dans GET /data : la trame WS, poussée toutes les ~5 s, REMPLACE
  // `latestSensorData` en entier (data/app.js, aucune fusion), donc les champs
  // repassaient à `undefined` en quelques secondes. Deux champs SEULEMENT sont
  // servis ici (périmètre arrêté après chiffrage) ; les sept autres
  // (`active`, `pump`, `phase`, `remaining_s`, `purges_left`, `purge_s`,
  // `measure_s`) ne pilotent l'écran de l'assistant que PENDANT une session et
  // restent /data-seuls, avec un rafraîchissement forcé côté client.
  //
  // POURQUOI CE N'EST PAS COSMÉTIQUE — `aborted` EST UN GARDE-FOU DE DOSAGE.
  // C'est le drapeau « mesure de débit interrompue par le SERVEUR » (perte de
  // filtration), lu par saveMeasuredFlow() côté client pour REFUSER de router
  // une mesure avortée vers la saisie du volume. Aujourd'hui il est MORT en
  // pratique : la trame WS écrase `latestSensorData` avant la saisie.
  // Un débit mesuré sous-estimé agit DEUX FOIS DANS LE MÊME SENS :
  //   1. planPulse (src/pulse_dosing_logic.cpp:45) : pulseMs = periodMs ×
  //      meanFlow / ampFlow → un ampFlow trop petit ALLONGE le pouls, donc
  //      sur-injection RÉELLE ;
  //   2. pulseDeltaVolumeMl (:161) crédite le plateau au MÊME ampFlow, donc
  //      dailyInjectedMl SOUS-COMPTE d'autant et la garde maxDailyMl est
  //      DESSERRÉE du même facteur — la sécurité volumique se ment à elle-même.
  // Le filet en aval est partiel : les bornes acceptent 0 < flow ≤ 200, donc
  // 12 mL/min au lieu de 26 passe sans bruit. Avant de juger qu'un drapeau
  // d'assistant n'a rien à faire dans la trame capteurs : c'est CE motif qu'il
  // faut réfuter, pas un affichage.
  //
  // `flow255` reste un TABLEAU DE 2 ÉLÉMENTS, jamais un scalaire : un scalaire
  // retomberait SILENCIEUSEMENT sur le repli côté client (data/app.js:8996,
  // :9712). Mêmes règles null et mêmes arrondis que /data (aucun des deux :
  // constante d'usine et booléen, jamais NaN) — cf. src/web_routes_data.cpp,
  // bloc `faObj`, dont le NOM DU RECEVEUR est repris à l'identique ici : la
  // sentinelle de parité extrait les champs imbriqués des deux côtés avec UN
  // SEUL extracteur textuel, qu'un nom différent casserait.
  // Snapshot sans mutex (getFlowAssistStatus, patron manualInject*).
  {
    FlowAssistStatus fa = getFlowAssistStatus();
    JsonObject faObj = d["flow_assist"].to<JsonObject>();
    faObj["aborted"] = fa.aborted;
    JsonArray f255 = faObj["flow255"].to<JsonArray>();
    f255.add(kPumpFlow255MlPerMin);
    f255.add(kPumpFlow255MlPerMin);
  }

  d["lighting_enabled"] = lighting.isOn();  // état réel du relais, pas lightingCfg.enabled

  // feature-053 : Mode Boost — état effectif (isBoostActive expire à minuit) + epoch
  // d'expiration (0 si inactif). Le client calcule le temps restant.
  {
    time_t nowEpoch = time(nullptr);
    d["boost_active"] = isBoostActive(nowEpoch);
    d["boost_until"]  = (long)boostState.untilEpoch;
    // feature-055 : leviers réellement actifs du Boost (affichage persistant du widget)
    // feature-056 : filtration prolongée seulement si PC gère la filtration (Managed).
    d["boost_filtration_extended"] = (mqttCfg.installMode == InstallMode::ManagedFiltration);
    d["boost_chlorine_boosted"]    = (mqttCfg.orpRegulationMode == "automatic");
  }

  // feature-056 : présence d'eau résolue (source UNIQUE) + mode d'installation.
  // Hooks pour l'UI (source/fraîcheur du signal filtration) — champs affinables
  // par web-ui-developer.
  {
    WaterPresence wp = filtration.resolveWaterPresence();
    d["install_mode"]  = installModeToString(mqttCfg.installMode);
    d["water_present"] = wp.waterPresent;
    d["filtration_state_stale"] = wp.stale;
    const char* src = "commanded";
    switch (wp.source) {
      case WaterSource::FiltrationCommanded: src = "commanded"; break;
      case WaterSource::PoweredAssumed:      src = "powered";   break;
      case WaterSource::ExternalSignal:      src = "external";  break;
    }
    d["filtration_state_source"] = src;
    // feature-056 : détail du signal externe pour la pill UI (mode external) —
    // distingue « Arrêtée » (OFF connu) de « Aucun signal » (boot) et donne l'âge.
    bool extOn = false, extKnown = false;
    uint32_t extLastMs = 0;
    filtration.getExternalState(extOn, extLastMs, extKnown);
    d["filtration_ext_known"] = extKnown;
    d["filtration_ext_on"]    = extOn;
    d["filtration_ext_age_s"] = extKnown ? (uint32_t)(((uint32_t)millis() - extLastMs) / 1000UL) : 0;
  }

  // feature-061 : validité des mesures pH/ORP (cache measure_validity, même
  // source que /data). Les champs live ph/orp/phRaw/... restent publiés tels
  // quels (calibration, onglet Avancé) — l'UI arbitre via measures_valid.
  // NB : nom distinct de stabilization_remaining_s (stabilisation post-
  // calibration des pompes, plus haut) : ici purge de la chambre de mesure.
  // feature-091 (v2.34.0) : purge adaptative — écoulé, dernière durée, motif de
  // fin et nombre de re-purges. PARITÉ /data ↔ WS : mêmes clés que
  // web_routes_data.cpp (un champ WS-seul CLIGNOTERAIT à chaque rafraîchissement).
  {
    const MeasureValiditySnapshot mv = measureValidity.getSnapshot();  // snapshot cohérent
    d["measures_valid"] = mv.measuresValid;
    d["measures_stab_elapsed_s"] = mv.stabElapsedS;
    d["measures_purge_last_duration_s"] = mv.lastPurgeDurationS;
    d["measures_purge_last_end"] = mv.lastPurgeEnd;
    d["measures_purge_count"] = mv.purgeCount;
  }

  // feature-096 (v2.37.0) : TEMPS D'ATTEINTE DE CIBLE — 8 champs servis depuis
  // l'instantané mémorisé de timeToTarget (copie sous spinlock portMUX). Ce
  // chemin n'appelle JAMAIS PumpController::getPhDecisionValue() : cet
  // accesseur est réservé à loopTask (condition C2 du gate feature-096).
  // PARITÉ /data ↔ WS obligatoire : MÊMES clés, MÊMES arrondis (pH 2 décimales,
  // ORP 1 décimale), MÊME règle NaN → null que web_routes_data.cpp, publiées
  // dans le MÊME passage. Un champ servi d'un seul côté ne disparaît pas, il
  // CLIGNOTE (`latestSensorData` est alimenté par le WS puis intégralement
  // REMPLACÉ par les charges /data toutes les ~30 s) — 3 récidives documentées.
  //   *_ttt_state = 0 pas de départ / 1 en cours / 2 atteinte / 3 non atteinte
  //   *_ttt_min   = minutes de FILTRATION (cf. état), bornées à 1440
  //   *_ttt_e0    = écart initial SIGNÉ à t0 (null si aucun départ)
  //   *_ttt_flag  = CHAMP DE BITS (0x01 consigne modifiée, 0x02 boost, 0x04
  //                 purge au plafond, 0x08 reprise secteur, 0x10 régulation non
  //                 automatique) : plusieurs causes SIMULTANÉES possibles, bits
  //                 5-7 libres — le client teste des bits, jamais une égalité.
  {
    const TttSnapshot ttt = timeToTarget.getSnapshot();
    d["ph_ttt_state"] = ttt.ph.state;
    d["ph_ttt_min"]   = ttt.ph.minutes;
    if (!isnan(ttt.ph.e0)) d["ph_ttt_e0"] = round(ttt.ph.e0 * 100.0f) / 100.0f; else d["ph_ttt_e0"] = nullptr;
    d["ph_ttt_flag"]  = ttt.ph.flags;
    d["orp_ttt_state"] = ttt.orp.state;
    d["orp_ttt_min"]   = ttt.orp.minutes;
    if (!isnan(ttt.orp.e0)) d["orp_ttt_e0"] = round(ttt.orp.e0 * 10.0f) / 10.0f; else d["orp_ttt_e0"] = nullptr;
    d["orp_ttt_flag"]  = ttt.orp.flags;
  }

  // >>> PARITE-DATA:DEBUT
  // CONVENTION NORMATIVE (lue telle quelle par la sentinelle de parité, sur le
  // patron des marqueurs PARITE-CONFIG de web_routes_config.cpp / ws_manager.cpp) :
  // entre ces deux marqueurs, TOUTE clé s'écrit `d["<clé>"]` — un seul
  // receveur. L'autre côté du contrat porte les MÊMES clés entre les MÊMES
  // marqueurs. Une clé posée hors des marqueurs est INVISIBLE pour la sentinelle :
  // la parité cesserait d'être garantie sans que rien ne casse.
  //
  // POURQUOI CE BLOC EXISTE (feature-092). Un champ servi d'un seul côté ne
  // disparaît pas, il CLIGNOTE : `latestSensorData` est alimenté par le WS, puis
  // intégralement REMPLACÉ par les charges /data toutes les ~30 s. Trois
  // récidives documentées en trois versions ; la pause mélange était encore dans
  // ce cas (WS-seule) — d'où sa migration DANS ce bloc.
  //
  // PAUSE MÉLANGE — les quatre champs qui manquaient à /data. La pastille d'état
  // de la carte « Débit d'injection » les consomme : sans eux, elle basculerait
  // de « Pause mélange · 12 min » à « En attente » à chaque charge /data.
  d["phMixingDelayActive"]  = PumpController.isPhMixingDelayActive(nowMs);
  d["orpMixingDelayActive"] = PumpController.isOrpMixingDelayActive(nowMs);
  d["ph_mix_remaining_s"]   = PumpController.getMixingRemainingS(0);
  d["orp_mix_remaining_s"]  = PumpController.getMixingRemainingS(1);
  //
  // COMPOSITION DU DÉBIT D'INJECTION (feature-092) — l'équation que la régulation
  // applique déjà, rendue lisible : demande = dose de base apprise + écart
  // courant + rattrapage accumulé, bornée par le débit soutenable.
  //   ph_ff_learned_ml_per_min   = dose de base APPRISE (mL/min, 2 décimales),
  //       min(charge médiane 7 j, 60 % du soutenable) — calculée À LA LECTURE :
  //       elle reste vraie hors session, contrairement à `ph_ff_ml_per_min` qui
  //       vaut 0 hors session (les deux coexistent, aucun n'est renommé).
  //       null si < 3 journées exploitables ou pompe non caractérisée. 0,00 avec
  //       3 journées sans injection est une OBSERVATION, pas une indisponibilité.
  //   ph_p_term_ml_per_min       = écart courant kp·e, SIGNÉ (négatif = pH sous
  //       la consigne), 0 dans la bande morte, JAMAIS borné ; null si la mesure
  //       n'est pas représentative ou si kp vaut 0 (régulateur inerte : un 0,00
  //       s'y lirait « pH à la consigne » sur une régulation morte).
  //   ph_i_term_ml_per_min       = rattrapage SERVI (0 dans la bande morte) —
  //       distinct de `ph_integral_term`, terme BRUT et contrat externe
  //       historique, qui reste inchangé. Deux champs, deux sémantiques.
  //   ph_demand_ml_per_min       = demande TOTALE bornée = clamp(PI servi + FF,
  //       0, débit d'amplitude). La borne basse porte sur le COUPLE p + i, jamais
  //       sur la somme des trois termes (cf. doseComposition, dosing_logic.h).
  //   orp_p_term_ml_per_min      = idem ORP ; orp_demand_ml_per_min = demande
  //       (régulation P-PURE : Ki = 0, aucun feedforward — la demande VAUT le
  //       terme proportionnel borné, et la carte ORP montre ainsi POURQUOI le
  //       chlore est régulé plus simplement).
  //   orp_sustainable_ml_per_min = plafond de débit moyen de la voie ORP imposé
  //       par sa limite horaire (miroir de `ph_sustainable_ml_per_min`).
  //   orp_limited_by_hourly_budget = VERDICT firmware « le chlore est borné par
  //       sa limite horaire EN CE MOMENT » (booléen, jamais null) — homologue
  //       d'`ph_limited_by_hourly_budget`, qui n'existait pas côté ORP alors que
  //       c'est la voie qui rencontre le cas TOUS LES JOURS sur cet appareil
  //       (journal du 07/09 09:15 : « Limite horaire ORP atteinte : 10 min/h
  //       consommées »). Verdict, jamais une comparaison à refaire côté client.
  // Règle NaN → null EXPLICITE (absent ≠ 0) et arrondi 2 décimales pour les
  // sept — IDENTIQUES de l'autre côté, servis dans le MÊME passage.
  // Accesseurs LECTURE SEULE : aucune I²C, aucun LittleFS, aucune NVS, aucun
  // appel à getPhDecisionValue() (réservée à loopTask, condition C2 du gate
  // feature-096) — sûrs dans un handler AsyncWebServer.
  const float phFfLearned  = PumpController.getPhFeedforwardLearnedFlow();
  const float phPTerm      = PumpController.getPhPTermFlow();
  const float phITerm      = PumpController.getPhITermFlow();
  const float phDemand     = PumpController.getPhDemandFlow();
  const float orpPTerm     = PumpController.getOrpPTermFlow();
  const float orpDemand    = PumpController.getOrpDemandFlow();
  const float orpSust      = PumpController.getOrpSustainableMlPerMin();
  if (!isnan(phFfLearned)) d["ph_ff_learned_ml_per_min"] = round(phFfLearned * 100.0f) / 100.0f; else d["ph_ff_learned_ml_per_min"] = nullptr;
  if (!isnan(phPTerm))     d["ph_p_term_ml_per_min"]     = round(phPTerm * 100.0f) / 100.0f;     else d["ph_p_term_ml_per_min"]     = nullptr;
  if (!isnan(phITerm))     d["ph_i_term_ml_per_min"]     = round(phITerm * 100.0f) / 100.0f;     else d["ph_i_term_ml_per_min"]     = nullptr;
  if (!isnan(phDemand))    d["ph_demand_ml_per_min"]     = round(phDemand * 100.0f) / 100.0f;    else d["ph_demand_ml_per_min"]     = nullptr;
  if (!isnan(orpPTerm))    d["orp_p_term_ml_per_min"]    = round(orpPTerm * 100.0f) / 100.0f;    else d["orp_p_term_ml_per_min"]    = nullptr;
  if (!isnan(orpDemand))   d["orp_demand_ml_per_min"]    = round(orpDemand * 100.0f) / 100.0f;   else d["orp_demand_ml_per_min"]    = nullptr;
  if (!isnan(orpSust))     d["orp_sustainable_ml_per_min"] = round(orpSust * 100.0f) / 100.0f;   else d["orp_sustainable_ml_per_min"] = nullptr;
  d["orp_limited_by_hourly_budget"] = PumpController.isOrpLimitedByHourlyBudget();
  //
  // CARACTÉRISATION DE POMPE — deux booléens qui étaient /data-SEULS, donc le
  // piège n° 1 du projet dans son sens INVERSE : ce n'est pas la charge /data
  // qui écrasait un champ WS, c'est le push WebSocket qui effaçait un champ
  // /data de `latestSensorData`. Le pied « pompe non caractérisée » de la carte
  // « Débit d'injection » s'affichait donc une seconde puis disparaissait
  // jusqu'au rafraîchissement suivant. Même mécanisme, même correctif : la
  // PARITÉ, pas une mémorisation côté client. Booléens d'état STABLE (une pesée
  // au banc, un réglage de puissance), jamais null.
  d["ph_pump_flow_uncharacterized"]  = PumpController.isPhPumpFlowUncharacterized();
  d["orp_pump_flow_uncharacterized"] = PumpController.isOrpPumpFlowUncharacterized();
  // <<< PARITE-DATA:FIN

  d["time_synced"]   = (time(nullptr) >= kMinValidEpoch);
  d["uptime_ms"]     = millis();
  d["reset_reason"]  = getResetReason();
  // Statut MQTT poussé toutes les 5s pour rafraîchir le badge UI sans reload (feature-015).
  // Lit connectedAtomic (atomic relaxed) — pas de mutex nécessaire (cf. feature-014 IT2).
  d["mqtt_connected"] = mqttManager.isConnected();

  // bug-heap-toctou A2 : sérialisation directe dans le buffer statique fourni —
  // plus de String par push (churn G2). measureJson d'abord : si le JSON + NUL
  // ne tient pas → 0, l'appelant saute le push (jamais de troncature).
  if (measureJson(doc) + 1 > cap) return 0;
  return serializeJson(doc, buf, cap);
}

size_t WsManager::_buildConfigJson(char* buf, size_t cap) const {
  // feature-053 : +2 champs boost_active/boost_until → marge portée à 2304.
  // feature-067 : +1 champ ph_offset (~24 octets) → marge portée à 2368.
  // feature-074 : +6 champs caractérisation pompes (~194 o au pire) → 2560.
  //
  // bug-089 (v2.39.0) — PARITÉ DE LA TRAME `config` AVEC /get-config.
  // CRITÈRE RETENU, qui structurera tous les ajouts futurs : la trame porte
  // l'état MUTABLE À L'EXÉCUTION et NON DÉRIVABLE ; tout le reste est HTTP-seul
  // et DÉCLARÉ (table `kHttpOnlyKeys` de test/test_native_ws_config_parity).
  // La parité INTÉGRALE aurait été pire que le bug : +1365 o sur une trame
  // plafonnée DUR à kWsFrameBufBytes = 3584 o, et un dépassement fait SAUTER le
  // push (la garde n'a jamais tronqué) — on aurait remplacé des champs manquants
  // par une trame absente pour TOUS les clients.
  // 15 clés ajoutées ci-dessous ; octets SÉRIALISÉS au pire cas, comptés clé par
  // clé (guillemets + deux-points + valeur + virgule) :
  //   "max_cycles_per_day":60,                 → 1+18+1+1+ 2+1 =  24 o
  //   "regulation_speed":"normal",             → 1+16+1+1+ 8+1 =  28 o
  //   "pool_size":"medium",                    → 1+ 9+1+1+ 8+1 =  21 o
  //   "ph_auto_manual_epoch":4294967295,       → 1+20+1+1+10+1 =  34 o
  //   "ph_auto_manual_reason":"…",             → 1+21+1+1+427+1 = 452 o
  //   "ph_tracking_enabled":false,             → 1+19+1+1+ 5+1 =  28 o
  //   "ph_container_ml":123456.78,             → 1+15+1+1+ 9+1 =  28 o
  //   "ph_total_injected_ml":123456.78,        → 1+20+1+1+ 9+1 =  33 o
  //   "ph_remaining_ml":123456.78,             → 1+15+1+1+ 9+1 =  28 o
  //   "ph_alert_threshold_ml":123456.78,       → 1+21+1+1+ 9+1 =  34 o
  //   "orp_tracking_enabled":false,            → 1+20+1+1+ 5+1 =  29 o
  //   "orp_container_ml":123456.78,            → 1+16+1+1+ 9+1 =  29 o
  //   "orp_total_injected_ml":123456.78,       → 1+21+1+1+ 9+1 =  34 o
  //   "orp_remaining_ml":123456.78,            → 1+16+1+1+ 9+1 =  29 o
  //   "orp_alert_threshold_ml":123456.78,      → 1+22+1+1+ 9+1 =  35 o
  //   → +866 o au pire cas, dont 452 o pour la SEULE valeur à longueur non
  //     triviale, `ph_auto_manual_reason` : c'est un LITTÉRAL construit par
  //     pump_controller, JAMAIS saisi par l'utilisateur.
  //     ⚠️ CHIFFRE RECTIFIÉ EN REVUE (bug-089) : ce bloc annonçait « ~140
  //     caractères / 167 o ». C'était FAUX d'un facteur 2,7. Les chaînes réelles
  //     ont été reconstruites depuis leur unique écrivain, setPhAutoManualTrace()
  //     (pump_controller.cpp:1021-1041) : la branche « Diagnostic sonde EZO
  //     NORMAL » fait 411 caractères, soit 427 octets UTF-8 ; la branche « hors
  //     norme » 325 caractères / 340 o. Ne pas se fier à une estimation ici : la
  //     seule source de vérité est le littéral du firmware.
  // feature-110 : +2 clés, compteurs de DÉMARRAGES de pompe (dénominateur du
  // biais de comptage, comptés au pire cas uint32 saturé) :
  //   "ph_starts":4294967295,                  → 1+ 9+1+1+10+1 =  23 o
  //   "orp_starts":4294967295,                 → 1+10+1+1+10+1 =  24 o
  //   → +47 o au pire cas. Les 4 clés d'HYGIÈNE (`ph_anom`, `orp_anom`,
  //     `ph_anom_ml`, `orp_anom_ml`) sont volontairement HTTP-seules et
  //     déclarées comme telles dans kHttpOnlyKeys : les pousser en continu
  //     coûterait ~120 o de trame pour des valeurs lues UNE fois par campagne,
  //     au changement de bidon.
  // TAILLE SÉRIALISÉE de la trame complète (83 clés, format compact) : 2100 o en
  // nominal, ≈ 2995 o au PIRE CAS (chaînes libres à 64 caractères, uint32
  // saturés, flottants à 9 chiffres, raison de bascule à 427 o) → MARGE ≈ 589 o
  // sous le seul plafond DUR, sWsFrameBuf (kWsFrameBufBytes = 3584 o,
  // constants.h). Gabarit documentaire porté 2944 → 3008.
  // POURQUOI CE CHIFFRE COMPTE : c'est lui que lira le prochain contributeur pour
  // arbitrer son ajout. Avec l'ancienne valeur, il aurait cru disposer de 285 o
  // qui n'existent pas. Et le pire cas de longueur coïncide avec l'ESCALADE DE
  // SÉCURITÉ pH — précisément le moment où perdre la trame coûte le plus cher,
  // puisque la garde ne tronque pas : elle ABANDONNE le push.
  // NB IMPORTANT : en ArduinoJson 7, StaticJson<N> dérive de JsonDocument et N
  // n'est plus qu'une valeur DOCUMENTAIRE (capacity()) — le pool est alloué
  // dynamiquement, ce bump coûte donc 0 o de RAM. Le seul plafond DUR reste
  // kWsFrameBufBytes, et la garde `measureJson(doc) + 1 > cap` en fin de
  // fonction SAUTE le push (elle ne tronque JAMAIS) en incrémentant
  // _oversizedFrames (warn throttlé) — d'où l'exigence de marge ci-dessus.
  // RÉSERVE CONNUE : `server`, `topic`, `username` et `wifi_ssid` ne sont bornés
  // NULLE PART (/save-config ne valide aucune longueur) ; la marge est prouvée
  // sous l'hypothèse d'environ 64 o par champ. Surveiller oversizedFrameCount().
  StaticJson<3008> doc;
  doc["type"] = "config";
  JsonObject d = doc["data"].to<JsonObject>();

  // >>> PARITE-CONFIG:DEBUT
  // CONVENTION NORMATIVE (lue telle quelle par la sentinelle
  // test/test_native_ws_config_parity) : entre ces deux marqueurs, TOUTE clé de
  // la trame `config` s'écrit `d["<clé>"]` — un seul receveur, `d`. Côté HTTP,
  // le receveur est `doc[` entre les mêmes marqueurs de web_routes_config.cpp.
  // Une clé posée hors des marqueurs est INVISIBLE pour la sentinelle : la
  // parité cesserait d'être garantie sans que rien ne casse.
  d["server"]           = mqttCfg.server;
  d["port"]             = mqttCfg.port;
  d["topic"]            = mqttCfg.topic;
  d["username"]         = mqttCfg.username;
  d["password"]         = mqttCfg.password.length() > 0 ? "******" : "";
  d["enabled"]          = mqttCfg.enabled;
  d["mqtt_connected"]   = mqttManager.isConnected();
  d["ph_target"]        = mqttCfg.phTarget;
  d["orp_target"]       = mqttCfg.orpTarget;
  d["ph_enabled"]       = mqttCfg.phEnabled;  // dérivé : true si mode != manual
  d["ph_regulation_mode"] = mqttCfg.phRegulationMode;
  d["ph_daily_target_ml"] = mqttCfg.phDailyTargetMl;
  d["ph_pump"]          = mqttCfg.phPump;
  d["orp_enabled"]      = mqttCfg.orpEnabled;  // miroir : true si orpRegulationMode != manual
  d["orp_regulation_mode"] = mqttCfg.orpRegulationMode;
  d["orp_daily_target_ml"] = mqttCfg.orpDailyTargetMl;
  d["max_orp_ml_per_day"]  = safetyLimits.maxChlorineMlPerDay;
  // feature-021 : orp_cal_valid dérivé de Cal,? (cache) plutôt que d'une date NVS supprimée.
  d["orp_cal_valid"]    = sensors.getOrpCalibrationPointsCached() >= 1;
  d["orp_pump"]         = mqttCfg.orpPump;
  d["pump1_max_duty_pct"] = mqttCfg.pump1MaxDutyPct;
  d["pump2_max_duty_pct"] = mqttCfg.pump2MaxDutyPct;
  // feature-074 : parité STRICTE avec /get-config (mêmes noms). Sans ces clés,
  // la trame config WS écrasait l'affichage de la calibration côté UI : le
  // frontend traitait « clé absente » comme « zéro » — même leçon que la
  // parité /data des pentes (2.21.2). Absent = pas d'information, jamais 0.
  d["pump1_calibrated_duty"]        = mqttCfg.pump1CalibratedDuty;
  d["pump1_pulse_flow_ml_per_min"]  = mqttCfg.pump1PulseFlowMlPerMin;
  d["pump2_calibrated_duty"]        = mqttCfg.pump2CalibratedDuty;
  d["pump2_pulse_flow_ml_per_min"]  = mqttCfg.pump2PulseFlowMlPerMin;
  d["pump1_flow_weighed"]           = mqttCfg.pump1FlowWeighed;
  d["pump2_flow_weighed"]           = mqttCfg.pump2FlowWeighed;
  d["ph_limit_minutes"] = mqttCfg.phInjectionLimitMinutes;
  d["orp_limit_minutes"] = mqttCfg.orpInjectionLimitMinutes;
  // bug-089 : garde anti-cycling (feature-060) — mutable à l'exécution.
  d["max_cycles_per_day"] = pumpProtection.maxCyclesPerDay;
  d["install_mode"]     = installModeToString(mqttCfg.installMode);  // feature-056
  // bug-089 : vitesse de correction + taille de bassin. `pool_size` est aussi ce
  // qui permet au client de DÉRIVER `regulation_defaults` (HTTP-seul) depuis
  // `regulation_defaults_by_size`, déjà en cache côté app.js.
  d["regulation_speed"] = mqttCfg.regulationSpeed;
  d["pool_size"]        = poolSizeToString(mqttCfg.poolSize);  // feature-090
  d["ph_correction_type"] = mqttCfg.phCorrectionType;
  d["time_use_ntp"]     = mqttCfg.timeUseNtp;
  d["ntp_server"]       = mqttCfg.ntpServer;
  d["manual_time"]      = mqttCfg.manualTimeIso;
  d["timezone_id"]      = mqttCfg.timezoneId;
  d["filtration_mode"]  = filtrationCfg.mode;  // feature-056 : filtration_enabled → install_mode
  d["filtration_start"] = filtrationCfg.start;
  d["filtration_end"]   = filtrationCfg.end;
  d["filtration_running"] = filtration.isRunning();
  d["lighting_feature_enabled"] = lightingCfg.featureEnabled;
  d["lighting_enabled"] = lightingCfg.enabled;
  d["lighting_brightness"] = lightingCfg.brightness;
  d["lighting_schedule_enabled"] = lightingCfg.scheduleEnabled;
  d["lighting_start_time"] = lightingCfg.startTime;
  d["lighting_end_time"]   = lightingCfg.endTime;
  d["wifi_ssid"]        = WiFi.SSID();

  wifi_mode_t mode = WiFi.getMode();
  String ip;
  if (mode == WIFI_MODE_AP)         ip = WiFi.softAPIP().toString();
  else if (mode == WIFI_MODE_APSTA) ip = WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  else                              ip = WiFi.localIP().toString();
  d["wifi_ip"]          = ip;
  d["wifi_mode"]        = mode == WIFI_MODE_AP ? "AP" : (mode == WIFI_MODE_APSTA ? "AP+STA" : "STA");
  d["mdns_host"]        = kMdnsFullHost;
  d["max_ph_ml_per_day"]      = safetyLimits.maxPhMlPerDay;
  d["max_chlorine_ml_per_day"]= safetyLimits.maxChlorineMlPerDay;
  // feature-021 : statut calibration EZO depuis le cache Cal,? (lecture sans I²C).
  // ph_cal_valid = au moins 1 point calibré ; les détails (mid/low) sont dans phCalPoints.
  d["ph_cal_valid"]     = sensors.getPhCalibrationPointsCached() >= 1;
  d["ph_cal_points"]    = sensors.getPhCalibrationPointsCached();
  d["orp_cal_points"]   = sensors.getOrpCalibrationPointsCached();
  // feature-067 : offset logiciel de mesure pH (distinct de la calibration EZO).
  d["ph_offset"]        = mqttCfg.phOffset;
  d["temp_calibration_offset"]   = mqttCfg.tempCalibrationOffset;
  d["temp_calibration_date"]     = mqttCfg.tempCalibrationDate;
  d["temperature_enabled"]       = mqttCfg.temperatureEnabled;
  d["auth_enabled"]        = authCfg.enabled;
  d["sensor_logs_enabled"] = authCfg.sensorLogsEnabled;
  d["debug_logs_enabled"]  = authCfg.debugLogsEnabled;
  d["auth_password"]       = authCfg.adminPassword.length() > 0 ? "******" : "";
  d["auth_token"]          = authCfg.apiToken.length() > 8 ? (authCfg.apiToken.substring(0, 8) + "...") : "";
  d["time_current"]        = getCurrentTimeISO();
  // feature-053 : Mode Boost (état effectif + epoch d'expiration, 0 si inactif).
  d["boost_active"]        = isBoostActive(time(nullptr));
  d["boost_until"]         = (long)boostState.untilEpoch;

  // bug-089 : bascule AUTOMATIQUE de la régulation pH en manuel (v2.20.3, C5).
  // État persistant ÉCRIT PAR LE FIRMWARE (pump_controller) sans action de
  // l'utilisateur : sans ces deux clés, un client tiers n'apprend la bascule de
  // sécurité qu'à son prochain /get-config.
  d["ph_auto_manual_epoch"]  = mqttCfg.phAutoManualEpoch;
  d["ph_auto_manual_reason"] = mqttCfg.phAutoManualReason;

  // bug-089 : suivi des volumes de produit. `*_total_injected_ml` avance à
  // CHAQUE injection et `*_remaining_ml` en découle — c'est l'état le plus
  // mutable de toute la config ; le laisser hors trame gelait la jauge de bidon
  // des clients tiers jusqu'à leur prochain /get-config.
  const float phRemaining  = max(0.0f, productCfg.phContainerVolumeMl  - productCfg.phTotalInjectedMl);
  const float orpRemaining = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  d["ph_tracking_enabled"]   = productCfg.phTrackingEnabled;
  d["ph_container_ml"]       = productCfg.phContainerVolumeMl;
  d["ph_total_injected_ml"]  = productCfg.phTotalInjectedMl;
  d["ph_remaining_ml"]       = phRemaining;
  d["ph_alert_threshold_ml"] = productCfg.phAlertThresholdMl;
  d["orp_tracking_enabled"]   = productCfg.orpTrackingEnabled;
  d["orp_container_ml"]       = productCfg.orpContainerVolumeMl;
  d["orp_total_injected_ml"]  = productCfg.orpTotalInjectedMl;
  d["orp_remaining_ml"]       = orpRemaining;
  d["orp_alert_threshold_ml"] = productCfg.orpAlertThresholdMl;
  // feature-110 : compteurs de DÉMARRAGES de pompe — état MUTABLE À
  // L'EXÉCUTION et NON DÉRIVABLE, donc dans la trame au même titre que
  // `*_total_injected_ml`, dont ils sont le dénominateur. Sans eux en WS, la
  // valeur affichée à côté du volume gèlerait entre deux /get-config alors que
  // le volume, lui, avancerait : le rapport V/N lu à l'écran serait FAUX (et
  // faux dans le sens qui gonfle δ). Les compteurs d'HYGIÈNE (`*_anom`,
  // `*_anom_ml`) restent HTTP-seuls : ils ne sont consultés qu'au relevé.
  d["ph_starts"]             = productCfg.phStartCount;
  d["orp_starts"]            = productCfg.orpStartCount;
  // <<< PARITE-CONFIG:FIN
  //
  // CLÉS VOLONTAIREMENT HTTP-SEULES (chacune déclarée AVEC son motif dans
  // `kHttpOnlyKeys`, test/test_native_ws_config_parity — la sentinelle casse si
  // l'une d'elles perd son motif ou disparaît de /get-config) :
  //   - ph_target_min/max/step, ph_offset_min/max/step,
  //     regulation_defaults_by_size : CONSTANTES DE COMPILATION, immuables à
  //     l'exécution (exclusion déjà argumentée dans web_routes_config.cpp) ;
  //   - regulation_defaults : DÉRIVABLE côté client — `pool_size` est désormais
  //     dans la trame et `regulation_defaults_by_size` est en cache (app.js,
  //     syncRegulationDefaults). La sortir coûterait +703 o de trame pour une
  //     information que le client sait reconstruire.

  // bug-heap-toctou A2 : même contrat que _buildSensorJson (0 = trame trop grande).
  if (measureJson(doc) + 1 > cap) return 0;
  return serializeJson(doc, buf, cap);
}
