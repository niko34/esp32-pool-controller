#include "web_routes_sensor_id.h"
#include "web_helpers.h"
#include "auth.h"
#include "logger.h"
#include "sensors.h"
#include "constants.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>

// =============================================================================
// GET /sensors/onewire/scan
// Liste les sondes DS18B20 détectées avec leur dernière T° brute en cache,
// leur adresse ROM (hex majuscule) et leur rôle (water/circuit/unknown).
//
// IMPORTANT : on NE déclenche PAS requestTemperatures() ici. Une conversion
// 12 bits prend ~750 ms, ce qui dépasse largement le budget des handlers
// AsyncWebServer (50 ms). On lit uniquement le cache alimenté par
// SensorManager::update() (cycle 2 s) — la fraîcheur est suffisante pour
// le polling UI 2 s.
// =============================================================================
static void handleScan(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  JsonDocument doc;
  JsonArray sondes = doc["sondes"].to<JsonArray>();

  uint8_t addrs[kMaxDs18b20Sondes][kSondeAddrLen];
  bool matched[kMaxDs18b20Sondes];
  sensors.getDetectedSondeAddresses(addrs, matched);
  int count = sensors.getDetectedSondeCount();

  for (int i = 0; i < count; ++i) {
    JsonObject s = sondes.add<JsonObject>();
    s["address"] = formatRomHex(addrs[i]);

    float t = sensors.getSondeTempRaw(i);
    if (isnan(t)) {
      s["temperature"] = nullptr;
    } else {
      s["temperature"] = round(t * 10.0f) / 10.0f;
    }

    SondeRole role = sensors.getSondeRole(i);
    const char* roleStr = "unknown";
    if (role == SondeRole::Water)        roleStr = "water";
    else if (role == SondeRole::Circuit) roleStr = "circuit";
    s["role"] = roleStr;
  }

  // Compter les sondes effectivement identifiées
  int identifiedCount = 0;
  for (int i = 0; i < count; ++i) {
    if (sensors.getSondeRole(i) != SondeRole::Unknown) identifiedCount++;
  }
  doc["identified_count"] = identifiedCount;
  doc["detected_count"] = count;

  sendJsonResponse(request, doc);
}

// =============================================================================
// POST /sensors/onewire/identify
// Body: { "address": "28FF1A2B3C4D5E6F", "role": "water" | "circuit" }
// Marque la sonde correspondante avec ce rôle. Auto-permutation si l'autre
// sonde portait déjà ce rôle.
// =============================================================================
static void handleIdentify(AsyncWebServerRequest* request, JsonVariant& json) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  JsonObject root = json.as<JsonObject>();
  String addressStr = root["address"] | "";
  String roleStr = root["role"] | "";

  if (addressStr.isEmpty() || roleStr.isEmpty()) {
    sendErrorResponse(request, 400, "Champs 'address' et 'role' requis");
    return;
  }

  uint8_t addr[kSondeAddrLen];
  if (!parseRomHex(addressStr, addr)) {
    sendErrorResponse(request, 400, "Adresse ROM invalide (16 caractères hex attendus)");
    return;
  }

  bool isWater;
  if (roleStr == "water") {
    isWater = true;
  } else if (roleStr == "circuit") {
    isWater = false;
  } else {
    sendErrorResponse(request, 400, "Rôle invalide (water|circuit attendu)");
    return;
  }

  if (!sensors.identifySonde(addr, isWater)) {
    sendErrorResponse(request, 404, "Adresse non détectée sur le bus OneWire");
    return;
  }

  JsonDocument doc;
  doc["success"] = true;
  sendJsonResponse(request, doc);
}

// =============================================================================
// POST /sensors/onewire/reset
// Body: {} — efface l'identification NVS et marque toutes les sondes Unknown.
// =============================================================================
static void handleReset(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  sensors.resetSondeIdentification();

  JsonDocument doc;
  doc["success"] = true;
  sendJsonResponse(request, doc);
}

void setupSensorIdRoutes(AsyncWebServer* server) {
  server->on("/sensors/onewire/scan", HTTP_GET, handleScan);

  auto* identifyHandler = new AsyncCallbackJsonWebHandler(
      "/sensors/onewire/identify", handleIdentify);
  identifyHandler->setMethod(HTTP_POST);
  server->addHandler(identifyHandler);

  // Le reset n'a pas de body utile : on accepte POST sans payload JSON.
  server->on("/sensors/onewire/reset", HTTP_POST, handleReset);
}
