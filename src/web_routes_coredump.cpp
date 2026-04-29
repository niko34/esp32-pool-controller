#include "web_routes_coredump.h"
#include "web_helpers.h"
#include "auth.h"
#include "logger.h"
#include <ArduinoJson.h>
#include <esp_core_dump.h>
#include <esp_partition.h>

static const esp_partition_t* getCoredumpPartition() {
  return esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

// Cause Xtensa → libellé lisible
static const char* excCauseStr(uint32_t cause) {
  switch (cause) {
    case 0:  return "IllegalInstruction";
    case 2:  return "InstructionFetchError";
    case 3:  return "LoadStoreError";
    case 6:  return "IntegerDivideByZero";
    case 9:  return "LoadStoreAlignmentCause";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "Unknown";
  }
}

// GET /coredump/info — résumé JSON du dernier crash
static void handleCoredumpInfo(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  JsonDocument doc;

  esp_core_dump_summary_t summary;
  if (esp_core_dump_get_summary(&summary) == ESP_OK) {
    doc["available"]          = true;
    doc["task"]               = summary.exc_task;
    doc["pc"]                 = summary.exc_pc;
    doc["exc_cause"]          = summary.ex_info.exc_cause;
    doc["exc_cause_str"]      = excCauseStr(summary.ex_info.exc_cause);
    doc["exc_vaddr"]          = summary.ex_info.exc_vaddr;
  } else {
    // Vérifier si la partition existe mais est vide
    const esp_partition_t* part = getCoredumpPartition();
    doc["available"] = false;
    doc["partition_found"] = (part != nullptr);
  }

  sendJsonResponse(request, doc);
}

// GET /coredump/download — binaire brut (pour espcoredump.py)
static void handleCoredumpDownload(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  const esp_partition_t* partition = getCoredumpPartition();
  if (!partition) {
    request->send(404, "text/plain", "Partition coredump absente");
    return;
  }

  // Valider que le coredump est présent et correct
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
    request->send(404, "text/plain", "Aucun coredump disponible");
    return;
  }

  // Streamer via AsyncCallbackResponse — pas d'allocation d'un buffer 64KB
  AsyncWebServerResponse* response = request->beginResponse(
    "application/octet-stream", size,
    [partition](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
      size_t toCopy = maxLen;  // maxLen <= size garanti par AsyncWebServer
      esp_partition_read(partition, index, buffer, toCopy);
      return toCopy;
    });
  response->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
  request->send(response);
  systemLogger.info("Coredump téléchargé (" + String(size) + " octets)");
}

// DELETE /coredump — effacer la partition pour le prochain crash
static void handleCoredumpErase(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  const esp_partition_t* partition = getCoredumpPartition();
  if (!partition) {
    sendErrorResponse(request, 404, "Partition coredump absente");
    return;
  }

  esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
  if (err == ESP_OK) {
    systemLogger.info("Partition coredump effacée");
    JsonDocument doc;
    doc["success"] = true;
    sendJsonResponse(request, doc);
  } else {
    sendErrorResponse(request, 500, "Erreur effacement coredump: " + String(err));
  }
}

void setupCoredumpRoutes(AsyncWebServer* server) {
  server->on("/coredump/info",     HTTP_GET,    handleCoredumpInfo);
  server->on("/coredump/download", HTTP_GET,    handleCoredumpDownload);
  server->on("/coredump",          HTTP_DELETE, handleCoredumpErase);
}
