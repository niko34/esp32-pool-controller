#ifndef WS_MANAGER_H
#define WS_MANAGER_H

#include <ESPAsyncWebServer.h>
#include <atomic>
#include "constants.h"  // kWsRegistrySlots
#include "logger.h"
#include "ws_client_registry_logic.h"  // bug-081 : registre PUR des clients authentifiés

// Gère le WebSocket /ws : authentification, push temps réel (capteurs, config, logs)
class WsManager {
public:
  void begin(AsyncWebServer* server);
  void update();  // À appeler dans loop() : cleanup + push capteurs toutes les 5s

  bool broadcastSensorData();
  bool broadcastConfig();
  bool broadcastLog(const LogRecord& entry);

  // À utiliser depuis un handler HTTP (tâche AsyncTCP) : marque le broadcast
  // pour exécution dans la main loop. Évite l'allocation d'un StaticJson<2048>
  // sur la pile de la tâche AsyncTCP qui a peu de stack.
  void requestConfigBroadcast();

  // bug-081, condition bloquante #3 (security-reviewer) : révocation immédiate
  // de TOUS les clients authentifiés. À appeler depuis le handler qui bascule
  // auth_enabled faux→vrai (src/web_routes_config.cpp) ou qui régénère le
  // token API (src/web_routes_auth.cpp POST /auth/regenerate-token). Sûre
  // depuis N'IMPORTE QUEL contexte (stores atomiques uniquement) : le registre
  // est vidé IMMÉDIATEMENT à l'appel ; la fermeture effective des sockets
  // (closeAll) est différée dans update() (loopTask), jamais exécutée ici.
  void revokeAllAuthenticated();

  bool hasClients() const;
  size_t clientCount() const;  // Nombre de clients WS connectés (diagnostic)
  uint32_t skippedPushCount() const;  // Cumul des pushes sautés (garde heap OU bad_alloc rattrapé)
  uint32_t oversizedFrameCount() const;  // Cumul des trames > kWsFrameBufBytes (push sauté, jamais tronqué)
  uint32_t registryFullCount() const;  // Cumul des connexions refusées (registre authentifié saturé)

private:
  AsyncWebSocket* _ws = nullptr;
  unsigned long _lastSensorPush = 0;
  bool _pendingConfigBroadcast = false;
  // bug-081 : remplace std::set<uint32_t> — un client NON authentifié ne doit
  // JAMAIS figurer dans ce registre, et c'est LUI SEUL (pas _ws->getClients())
  // qui détermine la cible des trois broadcasts (_sendToAuthenticated).
  WsClientRegistry<kWsRegistrySlots> _authRegistry;
  static constexpr unsigned long kSensorPushIntervalMs = 5000;

  // Époque d'authentification (bug-081) : incrémentée à chaque insertion RÉUSSIE
  // dans _authRegistry (kInserted — pas kAlreadyPresent, pas kFull). update()
  // compare cette époque à _servedEpoch, lue AVANT le snapshot, pour décider du
  // push "initial" (sensor + config) — remplace l'ancien booléen
  // _pendingInitialPush, qui ne distinguait pas "quel client" a motivé le
  // rattrapage (sans conséquence ici : le push est toujours broadcast à tout le
  // registre, comme avant).
  std::atomic<uint32_t> _authEpoch{0};
  // Condition bloquante #4 (security-reviewer) : avancée dès que le snapshot
  // pris dans update() était NON VIDE, MÊME SI la garde heap ou la saturation
  // (_registryFull) ont empêché tout envoi. Sans cela, en famine mémoire
  // prolongée, update() reconstruirait _buildSensorJson() à CHAQUE tour de
  // loop() au lieu d'attendre le prochain cycle de 5s — le client n'attend de
  // toute façon jamais plus de 5s (comportement inchangé).
  std::atomic<uint32_t> _servedEpoch{0};

  // bug-081 : révocation immédiate demandée (auth_enabled faux→vrai, rotation
  // de token) — traitée dans update() (loopTask) par closeAll(), jamais
  // directement depuis le handler HTTP qui l'a posée.
  std::atomic<bool> _pendingRevokeClose{false};

  // Compteur de connexions refusées par saturation du registre authentifié
  // (bug-081) : _authRegistry.insert() a renvoyé kFull. Incrémenté depuis
  // async_tcp (_onClientConnect / _onData), JAMAIS de log à ce site (contexte
  // async_tcp, pile réduite) — le warn throttlé vit exclusivement dans update().
  std::atomic<uint32_t> _registryFull{0};
  uint32_t _lastWarnedRegistryFull = 0;
  unsigned long _lastRegistryFullWarnMs = 0;

  // Compteur de pushes WS sautés par la garde heap (bug-badalloc-ws-broadcast) OU
  // par le catch de std::bad_alloc autour de l'envoi aux clients authentifiés
  // (bug-heap-toctou — filet TOCTOU). atomic relaxed (pattern connectedAtomic) :
  // incrémenté depuis loopTask ET potentiellement depuis le contexte de
  // l'appelant du log (broadcastLog via un futur callback logger) ; lu dans
  // update() (loopTask).
  std::atomic<uint32_t> _skippedPushes{0};
  uint32_t _lastWarnedSkipped = 0;   // Valeur du compteur au dernier warn émis
  unsigned long _lastSkipWarnMs = 0; // Instant du dernier warn (0 = jamais)

  // Compteur de trames dépassant le buffer statique kWsFrameBufBytes (bug-heap-toctou
  // A2) : push sauté (JAMAIS tronqué) — signale qu'il faut agrandir la constante.
  // Même patron atomic relaxed + warn throttlé dédié dans update().
  std::atomic<uint32_t> _oversizedFrames{0};
  uint32_t _lastWarnedOversized = 0;      // Valeur du compteur au dernier warn émis
  unsigned long _lastOversizedWarnMs = 0; // Instant du dernier warn (0 = jamais)

  // Garde heap commune des broadcasts (bug-badalloc-ws-broadcast) : false si le plus
  // gros bloc contigu RÉELLEMENT allouable (heapLargestFreeBlock8Bit, bug-080/v2.33.0)
  // est sous kWsPushMinAllocBytes (le push doit être sauté).
  // INVARIANT : ne logge JAMAIS — voir le commentaire d'implémentation.
  bool _heapAllowsPush();

  void _onEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                AwsEventType type, void* arg, uint8_t* data, size_t len);
  void _onClientConnect(AsyncWebSocketClient* client, AsyncWebServerRequest* request);
  void _onData(AsyncWebSocketClient* client, uint8_t* data, size_t len);

  // bug-081 : remplace les trois `_ws->textAll(...)`. `textAll()` diffusait à
  // TOUS les clients de /ws, authentifiés ou non — c'est le bug. Un seul
  // `std::make_shared<std::vector<uint8_t>>` est construit ICI (une allocation
  // par push. ⚠️ Revue : `textAll` n'en faisait PAS N (il partage un buffer
  // unique) — le profil est ÉGAL, pas meilleur ; c'est la surcharge
  // `text(id, const char*, len)` qui en aurait fait N via
  // makeSharedBuffer à chaque appel) puis partagé via `text(id, sharedBuffer)`
  // pour chaque id du snapshot. Snapshot pris sur la PILE (immunisé à une
  // mutation concurrente du registre pendant la boucle d'envoi — voir le
  // commentaire d'implémentation pour le scénario de réentrance PENDANT cette
  // même boucle). Retourne false si rien n'a été envoyé (registre vide, garde
  // heap, ou bad_alloc rattrapé) — les appelants (broadcastXxx) ne font plus
  // leur propre try/catch, il vit ICI, autour de TOUTE la boucle (au plus un
  // _skippedPushes++ par appel, pas un par client).
  bool _sendToAuthenticated(const char* buf, size_t len);

  // Construction des trames dans un buffer fourni (bug-heap-toctou A2 : buffer
  // statique kWsFrameBufBytes, plus de String par push). Retour : longueur du JSON
  // écrit (hors NUL), ou 0 si measureJson()+1 > cap — l'appelant SAUTE le push et
  // incrémente _oversizedFrames (jamais de troncature).
  size_t _buildSensorJson(char* buf, size_t cap) const;
  size_t _buildConfigJson(char* buf, size_t cap) const;
};

extern WsManager wsManager;

#endif // WS_MANAGER_H
