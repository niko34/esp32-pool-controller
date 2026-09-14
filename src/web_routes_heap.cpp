#include "web_routes_heap.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <cstdlib>  // strtoul — parsing addr/len en base 0 (accepte "0x..." et décimal)

#include "auth.h"               // REQUIRE_AUTH / RouteProtection / authManager
#include "constants.h"          // kHeapCensusPageEntries, kHttpHeapMarginBytes
#include "heap_census.h"        // heapCensusRequest/BeginRead/EndRead (R2), API R3 sous POOL_HEAP_PEEK
#include "heap_census_logic.h"  // heapPeekAsciiByte — module PUR (ADR-0017), utilisé par R3
#include "heap_metrics.h"       // heapLargestFreeBlock8Bit — garde 503 avant sérialisation (S10)
#include "sensor_trace_page.h"  // computeTracePage — pagination PURE réutilisée telle quelle

// =============================================================================
// PRODUCTEUR UNIQUE (vérifié dans ce lot, pas seulement supposé) : AsyncTCP.cpp
// (`.pio/libdeps/esp32dev_core3/AsyncTCP/src/AsyncTCP.cpp:191,320,380`) démarre
// UNE SEULE tâche FreeRTOS nommée "async_tcp" (`_async_service_task_handle`,
// créée sous garde `if (!_async_service_task_handle)`) : tous les handlers
// AsyncWebServer de ce projet — donc ceux-ci — s'exécutent sur cette tâche
// unique. L'hypothèse de heap_census.cpp (paramètres écrits AVANT la CAS, sans
// verrou, producteur unique) est donc VRAIE dans ce projet. Si ce projet
// démarrait un jour un second serveur/tâche async_tcp, cette hypothèse devrait
// être réévaluée.
//
// S13 : ZÉRO systemLogger dans ce fichier, chemins d'erreur compris — aucun
// #include "logger.h" ci-dessus, c'est délibéré. Tous les échecs se disent par
// code HTTP + champ JSON (bug-112 : la fenêtre forensique du journal tombe en
// ~27 requêtes, logger.h:50-51 — une route de diagnostic ne doit jamais
// pouvoir la consommer).
// =============================================================================

namespace {

// Compteur BOOKKEEPING de ce fichier de routes, distinct du `seq` firmware de
// HeapCensusSnapshot (généré par la loop à CHAQUE recensement ABOUTI). Publié
// sous `request_id` (jamais `seq`, pour ne pas laisser croire qu'il identifie
// un recensement complété) : permet à l'opérant de corréler une réponse 202
// avec les GET suivants, y compris quand la requête est encore REQUESTED.
std::atomic<uint32_t> sHeapCensusPostRequestId{0};

/**
 * @brief Marqueur d'exécution — quel binaire répond (S12, "marqueur d'exécution").
 * Coût négligeable (~30 o) : posé sur TOUTE réponse JSON de ce fichier, succès
 * ou erreur, pour pouvoir prouver après coup quel binaire tournait pendant la
 * fenêtre d'observation, même à partir d'une seule réponse d'erreur archivée.
 */
void addBuildMarker(JsonDocument& doc) {
  JsonObject b = doc["build"].to<JsonObject>();
  b["census"] = 1;
#if defined(POOL_HEAP_PEEK) && POOL_HEAP_PEEK == 1
  b["peek"] = 1;
#else
  b["peek"] = 0;
#endif
}

/** @brief Petite réponse JSON d'erreur, avec marqueur de build (helper commun). */
void sendJsonError(AsyncWebServerRequest* req, int code, const char* error) {
  JsonDocument doc;
  doc["error"] = error;
  addBuildMarker(doc);
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

/** @brief Variante avec en-tête Retry-After (429). */
void sendJsonErrorRetry(AsyncWebServerRequest* req, int code, const char* error,
                         uint32_t retryAfterSeconds) {
  JsonDocument doc;
  doc["error"] = error;
  addBuildMarker(doc);
  String out;
  serializeJson(doc, out);
  AsyncWebServerResponse* res = req->beginResponse(code, "application/json", out);
  res->addHeader("Retry-After", String(retryAfterSeconds));
  req->send(res);
}

// Revue code-reviewer : Retry-After DÉRIVÉ de kHeapCensusMinIntervalMs, jamais
// un littéral en dur — si la constante bouge, l'en-tête doit suivre tout seul.
// static_assert de cohérence : Retry-After s'exprime en secondes ENTIÈRES.
static_assert(kHeapCensusMinIntervalMs % 1000u == 0u,
              "kHeapCensusMinIntervalMs doit rester un multiple de 1000 ms pour "
              "que Retry-After (secondes entières) reste exact.");
constexpr uint32_t kHeapRoutesRetryAfterSeconds = kHeapCensusMinIntervalMs / 1000u;

/** @brief Traduction lisible du verdict de concordance (heap_census_logic.h). */
const char* concordanceLabel(uint8_t verdict) {
  switch (verdict) {
    case kHeapCensusVerdictMatch: return "match";
    case kHeapCensusVerdictMismatch: return "mismatch";
    default: return "undetermined";  // kHeapCensusVerdictUndetermined, ADR-0072
  }
}

/**
 * @brief Champs d'observabilité C4 (revue B2) — walk_us/max_walk_us/disabled.
 *
 * Lectures atomiques SANS effet de bord (heapCensusLastWalkUs/MaxWalkUs/
 * IsDisabled, déclarées par heap_census.h, PARTAGÉES R2/R3 : le chronomètre
 * est le même pour les deux). Posées sur GET /debug/heap_census, succès ET
 * erreur : c'est PRÉCISÉMENT au moment d'un 410 que l'opérant a besoin de
 * savoir si le module s'est auto-désactivé, et de combien le seuil a été
 * franchi — la base dérive (711 µs vs 516 µs six jours plus tôt), donc un 410
 * muet ne dit rien d'actionnable.
 *
 * ⚠️ Conflit avec la spec signalé, pas résolu ici : C4 (gate pool-chemistry)
 * demande aussi une « exposition MQTT », feature-128 liste « Topics MQTT :
 * Aucun ». Le minimum compatible avec S13 (zéro systemLogger, aucune
 * consommation du canal MQTT depuis une route de diagnostic) est CES CHAMPS
 * JSON — l'exposition MQTT, si elle est tranchée nécessaire, est hors du
 * périmètre de ce fichier de routes.
 */
void addCensusWalkInfo(JsonDocument& doc) {
  doc["walk_us"] = heapCensusLastWalkUs();
  doc["max_walk_us"] = heapCensusMaxWalkUs();
  doc["disabled"] = heapCensusIsDisabled();
}

/**
 * @brief Erreur JSON de GET /debug/heap_census, AVEC les champs C4 (B2).
 * Ne remplace PAS sendJsonError() (qui reste générique pour les autres
 * routes) : seule CETTE route publie walk_us/max_walk_us/disabled, y compris
 * sur ses réponses d'erreur (409/410) — c'est précisément là que l'opérant en
 * a besoin.
 */
void sendCensusJsonError(AsyncWebServerRequest* req, int code, const char* error) {
  JsonDocument doc;
  doc["error"] = error;
  addCensusWalkInfo(doc);
  addBuildMarker(doc);
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

}  // namespace

void setupHeapRoutes(AsyncWebServer* server) {
  if (server == nullptr) return;

  // ---------------------------------------------------------------------
  // POST /debug/heap_census?used_from=&used_to= — R2, demande un recensement
  // ---------------------------------------------------------------------
  server->on("/debug/heap_census", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
      sendJsonError(req, 403, "forbidden");
      return;
    }
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    uint32_t usedFrom = 0;
    uint32_t usedTo = 0;
    if (req->hasParam("used_from")) {
      usedFrom = (uint32_t)strtoul(req->getParam("used_from")->value().c_str(), nullptr, 0);
    }
    if (req->hasParam("used_to")) {
      usedTo = (uint32_t)strtoul(req->getParam("used_to")->value().c_str(), nullptr, 0);
    }

    HeapCensusRequestResult rr = heapCensusRequest(usedFrom, usedTo);
    switch (rr) {
      case kHeapCensusReqQueued: {
        uint32_t requestId = sHeapCensusPostRequestId.fetch_add(1, std::memory_order_relaxed) + 1u;
        JsonDocument doc;
        doc["queued"] = true;
        doc["request_id"] = requestId;
        addBuildMarker(doc);
        String out;
        serializeJson(doc, out);
        req->send(202, "application/json", out);
        return;
      }
      case kHeapCensusReqBusy:
        sendJsonError(req, 409, "busy");
        return;
      case kHeapCensusReqTooSoon:
        sendJsonErrorRetry(req, 429, "too_soon", kHeapRoutesRetryAfterSeconds);
        return;
      case kHeapCensusReqDisabled:
        sendJsonError(req, 410, "disabled");
        return;
    }
  });

  // ---------------------------------------------------------------------
  // GET /debug/heap_census?kind=free|used&offset=&count= — R2, instantané figé
  // ---------------------------------------------------------------------
  server->on("/debug/heap_census", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
      sendJsonError(req, 403, "forbidden");
      return;
    }
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    const HeapCensusSnapshot* snap = nullptr;
    HeapCensusReadResult rr = heapCensusBeginRead(&snap);
    if (rr != kHeapCensusReadOk) {
      // B2 : walk_us/max_walk_us/disabled même en erreur — un 410 MUET ne dit
      // rien d'actionnable si c'est C1 (auto-désactivation) qui en est la cause.
      if (rr == kHeapCensusReadStale) {
        sendCensusJsonError(req, 410, "stale");
      } else if (rr == kHeapCensusReadBusy) {
        sendCensusJsonError(req, 409, "busy");
      } else {
        sendCensusJsonError(req, 409, "not_ready");
      }
      return;
    }

    String kind = "free";
    if (req->hasParam("kind")) kind = req->getParam("kind")->value();
    const bool wantUsed = (kind == "used");

    size_t offset = 0;
    size_t count = 0;  // 0 => page pleine (computeTracePage)
    if (req->hasParam("offset")) {
      offset = (size_t)req->getParam("offset")->value().toInt();
    }
    if (req->hasParam("count")) {
      long c = req->getParam("count")->value().toInt();
      if (c > 0) count = (size_t)c;
    }

    const size_t total = wantUsed ? snap->usedReturned : snap->freeReturned;
    TracePageBounds page = computeTracePage(total, offset, count, kHeapCensusPageEntries);

    // ORDRE IMPÉRATIF (patron web_routes_debug.cpp:43) : doc -> measureJson ->
    // garde heap -> beginResponseStream. TOUTES les lectures de `snap` ont lieu
    // ICI, avant heapCensusEndRead() : `doc` en est une copie indépendante.
    JsonDocument doc;
    doc["seq"] = snap->seq;
    doc["took_at_ms"] = snap->tookAtMs;
    doc["heap_start"] = snap->heapStart;
    doc["heap_end"] = snap->heapEnd;
    doc["used_from"] = snap->usedFrom;
    doc["used_to"] = snap->usedTo;
    doc["free_block_count"] = snap->freeBlockCount;
    doc["used_block_count"] = snap->usedBlockCount;
    doc["used_in_window_count"] = snap->usedInWindowCount;
    doc["used_truncated"] = snap->usedTruncated;  // I13 : troncature du PARCOURS (fenêtre pleine)
    doc["floor_free_blocks"] = snap->floorFreeBlocks;
    doc["floor_used_blocks"] = snap->floorUsedBlocks;
    doc["concordance"] = concordanceLabel(snap->concordanceVerdict);
    doc["kind"] = wantUsed ? "used" : "free";
    doc["offset"] = (uint32_t)page.offset;
    doc["returned"] = (uint32_t)page.n;
    doc["total"] = (uint32_t)total;
    // I13 : troncature de PAGINATION, distincte de `used_truncated` ci-dessus
    // (troncature du PARCOURS, côté loop). Jamais silencieuse.
    doc["truncated"] = (page.offset + page.n) < total;

    JsonArray entries = doc["entries"].to<JsonArray>();
    if (wantUsed) {
      for (size_t i = 0; i < page.n; i++) {
        const HeapCensusUsedEntry& e = snap->usedBlocks[page.offset + i];
        JsonObject o = entries.add<JsonObject>();
        o["addr"] = e.addr;
        o["size"] = e.size;
        o["alloc_size"] = e.allocSize;  // kHeapCensusAllocSizeUnknown si canari non conforme (ADR-0072)
      }
    } else {
      for (size_t i = 0; i < page.n; i++) {
        const HeapCensusFreeEntry& e = snap->freeBlocks[page.offset + i];
        JsonObject o = entries.add<JsonObject>();
        o["addr"] = e.addr;
        o["size"] = e.size;
      }
    }
    addCensusWalkInfo(doc);  // B2 : walk_us/max_walk_us/disabled (C4)
    addBuildMarker(doc);

    // Plus aucune lecture de `snap` après ce point : READING -> IDLE peut être
    // relâché avant l'allocation lourde de sérialisation (measureJson + stream).
    heapCensusEndRead();

    size_t sz = measureJson(doc);
    if (sz + kHttpHeapMarginBytes > heapLargestFreeBlock8Bit()) {
      req->send(503, "application/json", "{\"error\":\"heap fragmenté, réessayez\"}");
      return;
    }
    AsyncResponseStream* res = req->beginResponseStream("application/json", sz);
    serializeJson(doc, *res);
    req->send(res);
  });

#if defined(POOL_HEAP_PEEK) && POOL_HEAP_PEEK == 1
  // ---------------------------------------------------------------------
  // POST /debug/heap_peek?addr=&len= — R3, NON LIVRÉ EN PRODUCTION (ADR-0081)
  // ---------------------------------------------------------------------
  server->on("/debug/heap_peek", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
      sendJsonError(req, 403, "forbidden");
      return;
    }
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    if (!req->hasParam("addr") || !req->hasParam("len")) {
      sendJsonError(req, 400, "missing_param");
      return;
    }
    uint32_t addr = (uint32_t)strtoul(req->getParam("addr")->value().c_str(), nullptr, 0);
    uint32_t len = (uint32_t)strtoul(req->getParam("len")->value().c_str(), nullptr, 0);

    HeapPeekRequestResult rr = heapPeekRequest(addr, len);
    switch (rr) {
      case kHeapPeekReqQueued: {
        JsonDocument doc;
        doc["queued"] = true;
        addBuildMarker(doc);
        String out;
        serializeJson(doc, out);
        req->send(202, "application/json", out);
        return;
      }
      case kHeapPeekReqBusy:
        sendJsonError(req, 409, "busy");
        return;
      case kHeapPeekReqTooSoon:
        sendJsonErrorRetry(req, 429, "too_soon", kHeapRoutesRetryAfterSeconds);
        return;
      case kHeapPeekReqDisabled:
        sendJsonError(req, 410, "disabled");
        return;
      case kHeapPeekReqBudgetExhausted:
        // Le budget (kHeapPeekMaxBytesPerBoot) borne un VIDAGE EN MASSE, pas la
        // divulgation d'un secret isolé (mot de passe WiFi, identifiants MQTT,
        // jeton API tiennent en < 200 o) — et /reboot est sous les MÊMES
        // identifiants : si l'authentification tombe, le budget tombe avec.
        // Il ne « borne » donc rien de plus que ce que S3 protège déjà.
        sendJsonError(req, 403, "budget_exhausted");
        return;
      case kHeapPeekReqInvalidLen:
        sendJsonError(req, 400, "invalid_len");
        return;
    }
  });

  // ---------------------------------------------------------------------
  // GET /debug/heap_peek — R3, hex + ASCII du dernier contenu lu
  // ---------------------------------------------------------------------
  server->on("/debug/heap_peek", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
      sendJsonError(req, 403, "forbidden");
      return;
    }
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    const HeapPeekSnapshot* snap = nullptr;
    HeapPeekReadResult rr = heapPeekBeginRead(&snap);
    if (rr != kHeapPeekReadOk) {
      if (rr == kHeapPeekReadStale) {
        sendJsonError(req, 410, "stale");
      } else if (rr == kHeapPeekReadBusy) {
        sendJsonError(req, 409, "busy");
      } else {
        sendJsonError(req, 409, "not_ready");
      }
      return;
    }

    JsonDocument doc;
    doc["seq"] = snap->seq;
    doc["took_at_ms"] = snap->tookAtMs;
    doc["addr"] = snap->addr;
    doc["alloc_size"] = snap->allocSize;
    doc["len"] = snap->len;
    doc["found"] = snap->found;
    doc["canary_valid"] = snap->canaryValid;
    doc["stable"] = snap->stable;
    doc["budget_clamped"] = snap->budgetClamped;  // I13 : jamais silencieux
    doc["budget_remaining"] = heapPeekBudgetRemaining();

    // S11 : hex + ASCII ne sont NI journalisés NI destinés à être versionnés —
    // c'est la responsabilité de l'opérant (note d'incident : adresse, taille,
    // identification, JAMAIS le contenu). Ce fichier se contente de servir la
    // réponse HTTP demandée par un opérant déjà authentifié CRITICAL.
    String hex;
    hex.reserve((size_t)snap->len * 2);
    String ascii;
    ascii.reserve((size_t)snap->len);
    for (uint32_t i = 0; i < snap->len; i++) {
      char buf[3];
      snprintf(buf, sizeof(buf), "%02x", snap->bytes[i]);
      hex += buf;
      ascii += heapPeekAsciiByte(snap->bytes[i]);
    }
    doc["hex"] = hex;
    doc["ascii"] = ascii;
    addBuildMarker(doc);

    // Plus aucune lecture de `snap` après ce point.
    heapPeekEndRead();

    size_t sz = measureJson(doc);
    if (sz + kHttpHeapMarginBytes > heapLargestFreeBlock8Bit()) {
      req->send(503, "application/json", "{\"error\":\"heap fragmenté, réessayez\"}");
      return;
    }
    AsyncResponseStream* res = req->beginResponseStream("application/json", sz);
    serializeJson(doc, *res);
    req->send(res);
  });
#endif  // POOL_HEAP_PEEK
}
