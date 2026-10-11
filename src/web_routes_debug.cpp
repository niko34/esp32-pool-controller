#include "web_routes_debug.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "auth.h"       // REQUIRE_AUTH / RouteProtection (revue 2.20.3)
#include "constants.h"
#include "heap_metrics.h"  // heapLargestFreeBlock8Bit — garde 503 sensor_trace (bug-080)
#include "logger.h"
#include "sensor_trace_page.h"
#include "sensors.h"

namespace {

// feature-152 : libellé DIAGNOSTIC seul (pas un contrat de sérialisation, au
// contraire de DoseRefusal) — sert à exposer en clair, sur l'endpoint de
// debug, le MÊME discriminant que celui qui qualifie les causes de refus de
// dosage (resolveFilterState(), dosing_logic.h).
const char* doseFilterStateLabel(DoseFilterState st) {
  switch (st) {
    case DoseFilterState::Ready:      return "ready";
    case DoseFilterState::Warmup:     return "warmup";
    case DoseFilterState::Frozen:     return "frozen";
    case DoseFilterState::Stale:      return "stale";
    case DoseFilterState::GateClosed: return "gate_closed";
    case DoseFilterState::Unknown:
    default:                          return "unknown";
  }
}

}  // namespace

// =============================================================================
// Endpoints HTTP
// =============================================================================

void setupDebugRoutes(AsyncWebServer* server) {
  if (server == nullptr) return;

  // ---------- GET /debug/sensor_trace ----------
  // feature-057 : dump du ring buffer de trace capteur (pH/ORP brut + lissé, T°).
  // PAGINÉ (bug-crash-sensor-trace-badalloc) : ?offset=N&count=M, M borné par
  // kSensorTraceMaxSamplesPerPage (~3,5 Ko/page) — jamais ~20 Ko contigus dans
  // le cbuf d'AsyncResponseStream (std::bad_alloc → abort() sur heap fragmenté).
  // Réponse : count (total buffer), offset (effectif clampé), returned (n de la
  // page), interval_ms, now, samples. Le glissement du ring buffer entre deux
  // pages est toléré (diagnostic, contrat lock-free SensorTraceSample).
  // Non bloquant (< 50 ms) : lecture lock-free, 50 objets max/page.
  server->on("/debug/sensor_trace", HTTP_GET, [](AsyncWebServerRequest* req) {
    size_t offset = 0;
    size_t count  = kSensorTraceMaxSamplesPerPage;
    if (req->hasParam("offset")) {
      offset = (size_t)req->getParam("offset")->value().toInt();
    }
    if (req->hasParam("count")) {
      long c = req->getParam("count")->value().toInt();
      if (c > 0) count = (size_t)c;  // 0 ou invalide → défaut (page max)
    }
    size_t total = sensors.getSensorTraceCount();
    TracePageBounds page =
        computeTracePage(total, offset, count, kSensorTraceMaxSamplesPerPage);

    // ORDRE IMPÉRATIF : doc → measureJson → garde heap → beginResponseStream.
    JsonDocument doc;
    doc["count"]       = total;
    doc["offset"]      = page.offset;
    doc["returned"]    = page.n;
    doc["interval_ms"] = (uint32_t)kPhOrpSensorIntervalMs;
    doc["now"]         = (uint32_t)millis();
    JsonArray arr = doc["samples"].to<JsonArray>();
    sensors.appendSensorTraceJson(arr, page.offset, page.n);

    // Garde-fou défensif : si même une page ne tient pas dans le plus gros
    // bloc libre (+ marge), 503 propre au lieu d'un bad_alloc → reboot.
    // bug-080 lot B-2 (ADR-0058) : DERNIÈRE garde restée sur ESP.getMaxAllocHeap(),
    // qui compte l'IRAM 32 bits inutilisable par le `new` du cbuf
    // d'AsyncResponseStream. Mesure basculée sur heapLargestFreeBlock8Bit()
    // (caps fixes MALLOC_CAP_INTERNAL|8BIT = ce que ce `new` peut réellement
    // obtenir). Seuil INCHANGÉ : on corrige la mesure, pas la marge. Écart
    // quasi nul en régime normal (27 636 vs 26 612 o le 05/09), mais la brute
    // reste figée en famine là où le 8 bits s'effondre (plancher relevé 14 324 o).
    size_t sz = measureJson(doc);
    if (sz + kSensorTraceHeapMarginBytes > heapLargestFreeBlock8Bit()) {
      req->send(503, "application/json",
                "{\"error\":\"heap fragmenté, réessayez\"}");
      return;
    }
    // Pré-dimensionné : une seule allocation du cbuf, pas de resize() en cours
    // de sérialisation.
    AsyncResponseStream* res = req->beginResponseStream("application/json", sz);
    serializeJson(doc, *res);
    req->send(res);
  });

  // ---------- POST /debug/sensor_trace_clear ----------
  // feature-057 : vide le ring buffer de trace (démarre une fenêtre propre).
  server->on("/debug/sensor_trace_clear", HTTP_POST, [](AsyncWebServerRequest* req) {
    sensors.clearSensorTrace();
    systemLogger.info("[Debug] Trace capteur vidée");
    req->send(200, "application/json", "{\"success\":true}");
  });

  // ---------- POST /debug/ph_slope_refresh ----------
  // feature-024 : force une re-query Slope,? sur l'EZO pH (utile pour valider
  // une nouvelle calibration sans attendre le cycle 24h).
  // Réponse 200 si la commande a été enfilée, 503 si la queue est saturée.
  server->on("/debug/ph_slope_refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
    bool ok = sensors.enqueuePhSlopeQuery();
    if (ok) {
      req->send(200, "application/json", "{\"success\":true,\"queued\":true}");
    } else {
      req->send(503, "application/json", "{\"error\":\"queue full or already pending\"}");
    }
  });

  // ---------- POST /debug/sensor_filter_reset[?sensor=ph|orp] ----------
  // feature-152 : reset des filtres pH/ORP (médiane + lissage). Repasse le
  // filtre visé en warmup → dosage auto bloqué tant que la fenêtre médiane
  // (kSensorFilterMedianWindow) n'est pas de nouveau pleine.
  //
  // v2.20.3 (D2) — DEUX correctifs :
  //   (a) SÉLECTIVITÉ : `?sensor=ph` ou `?sensor=orp` ne réinitialise QUE le
  //       filtre demandé. Sans paramètre, les DEUX filtres sont réinitialisés
  //       (compatibilité descendante stricte : même réponse `{"reset":["ph","orp"]}`
  //       qu'avant). Lors du déblocage en production du 2026-08-16, cette route a
  //       sacrifié le filtre ORP qui était parfaitement sain.
  //   (b) CONCURRENCE : l'ancienne version appelait resetPhFilter()/resetOrpFilter()
  //       DIRECTEMENT depuis le handler, donc depuis la tâche async_tcp — en
  //       violation du contrat « écriture loopTask exclusive » de sensor_filter.h
  //       (bug latent : écriture concurrente du buffer pendant un addSample()).
  //       On passe désormais par requestFilterReset(), consommé en tête de
  //       SensorManager::update(). La réponse devient donc « acceptée », pas
  //       « exécutée » : le reset a lieu au prochain tour de loop (< 100 ms).
  server->on("/debug/sensor_filter_reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    // REVUE 2.20.3 (bloquant n°2) : cette route effectue EXACTEMENT la même
    // action destructrice que /reset-sensor-filter (effacement de la détection
    // de sonde figée et du latch EMI), mais elle était ouverte SANS
    // authentification — elle contournait donc intégralement les gardes posées
    // ce cycle sur l'autre route. Même protection des deux côtés.
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    auto* p = req->getParam("sensor", true);
    if (!p) p = req->getParam("sensor");
    String sensorName = p ? p->value() : String();
    sensorName.toLowerCase();

    bool doPh = true, doOrp = true;   // défaut historique : les deux
    if (sensorName == "ph")       { doPh = true;  doOrp = false; }
    else if (sensorName == "orp") { doPh = false; doOrp = true;  }
    else if (sensorName.length() > 0) {
      req->send(400, "application/json",
                "{\"error\":\"invalid_sensor\",\"message\":\"'sensor' doit valoir ph ou orp\"}");
      return;
    }

    sensors.requestFilterReset(doPh, doOrp, "route de debug");
    // Le détail de l'état détruit (médiane avant/après, rejets, latch, sonde
    // figée) est journalisé par SensorManager au moment de l'exécution (D4).
    String reset = "[";
    if (doPh)  reset += "\"ph\"";
    if (doPh && doOrp) reset += ",";
    if (doOrp) reset += "\"orp\"";
    reset += "]";
    req->send(200, "application/json",
              "{\"success\":true,\"queued\":true,\"reset\":" + reset + "}");
  });

  // ---------- GET /debug/sensor_filter_state ----------
  // feature-152 : état brut JSON de la chaîne de mesure pH/ORP (diagnostic).
  // Lecture lock-free des getters SensorManager (contexte handler async —
  // valeurs scalaires atomiques). `state` est le MÊME discriminant qui qualifie
  // les causes de refus de dosage (DoseFilterState, dosing_logic.h) : il dit
  // POURQUOI `filtered` est NaN quand c'est le cas, sans dupliquer de logique.
  server->on("/debug/sensor_filter_state", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonObject ph = doc["ph"].to<JsonObject>();
    // feature-067 : `raw` = BRUT EZO (sans offset — c'est LA valeur de diagnostic
    // électrochimique) ; `median` et `filtered` sont CORRIGÉS (brut + offset), donc
    // identiques à ce que consomme la régulation. L'offset appliqué est lisible via
    // GET /get-config (champ ph_offset).
    float phRaw = sensors.getPhRaw();       // BRUT
    float phMed = sensors.getPhMedian();    // CORRIGÉ (diagnostic)
    float phFil = sensors.getPhFiltered();  // VALEUR OFFICIELLE, CORRIGÉE
    if (!isnan(phRaw)) ph["raw"] = round(phRaw * 1000.0f) / 1000.0f; else ph["raw"] = nullptr;
    if (!isnan(phMed)) ph["median"] = round(phMed * 1000.0f) / 1000.0f; else ph["median"] = nullptr;
    if (!isnan(phFil)) ph["filtered"] = round(phFil * 1000.0f) / 1000.0f; else ph["filtered"] = nullptr;
    ph["state"] = doseFilterStateLabel(sensors.getPhFilterState());
    ph["rejected"] = sensors.getPhRejectedCount();

    JsonObject orp = doc["orp"].to<JsonObject>();
    float orpRaw = sensors.getOrpRaw();
    float orpMed = sensors.getOrpMedian();
    float orpFil = sensors.getOrpFiltered();  // VALEUR OFFICIELLE
    if (!isnan(orpRaw)) orp["raw"] = round(orpRaw); else orp["raw"] = nullptr;
    if (!isnan(orpMed)) orp["median"] = round(orpMed); else orp["median"] = nullptr;
    if (!isnan(orpFil)) orp["filtered"] = round(orpFil); else orp["filtered"] = nullptr;
    orp["state"] = doseFilterStateLabel(sensors.getOrpFilterState());
    orp["rejected"] = sensors.getOrpRejectedCount();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });
}
